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
 * @file type_checker.cpp
 * @brief Implementacion del pase de tipos de Vex.
 *
 * Estructura:
 *   1. collect_globals() - pase superficial que llena el scope global
 *      con las funciones y variables top-level (permite forward refs).
 *   2. check_functions() - pase profundo que verifica cada cuerpo.
 *
 * Patrones de hardware aplicados:
 *   - lookup() recorre los scopes desde el mas interno al global con
 *     un loop cerrado; los scopes son std::unordered_map (los nombres
 *     son cadenas variables de tamanyo, std::vector ordenado seria
 *     peor para inserciones repetidas).
 *   - El AST se modifica solo en @c result_type; nada de allocaciones
 *     extra durante el checking.
 *   - La tabla de firmas de funciones es un vector contiguo con
 *     acceso por indice (cache-friendly al validar muchas llamadas).
 */

#include "vex/type_checker.h"
#include "vex/collection_intrinsics.h"  // tabla de tipos coleccion
#include "vex/comptime_introspect.h"    // comptime_field_type
#include "vex/lexer.h"                  // parse de fragments en comptime_emit_expr
#include "vex/parser.h"                 // parse_one_expr para macros con splice
#include "loader/oop_types.h"  // para sizeof(loader::ObjectHeader) en el layout de clases

#include <algorithm>
#include <utility>
#include <cstdlib>      // getenv para VESTA_MC_VMONLY/PREBUILT
#include <cstring>      // memcpy para bitcast f64 -> u64
#include <fstream>      // cargar prebuilt .velb desde disco
#include <iostream>     // log VESTA_MC_VERBOSE
#include <mutex>        // std::call_once para registro one-shot

#include "ffi/virtual_lib_registry.h"  // registrar vex_static_assert

/* extern "C" decl global del thunk generator.  La impl esta
 * en src/runtime/native_callback.cpp.  La registramos como virtual_fn
 * "vesta_runtime:vex_get_native_thunk" para que el builtin Vex
 * `as_native_callback(fn)` la invoque via CALLN. */
extern "C" uint64_t vex_get_native_thunk(uint64_t fn_pc, uint64_t argc);

namespace vex {

    /* TypeChecker activo (thread_local) para los virtual
     * fns expuestos a macros via `extern "vesta_comptime"`.  Cuando un
     * macro llama @c static_assert (o futuros @c comptime_compile, etc.),
     * el virtual fn accede al TypeChecker del compile en curso via este
     * puntero.  Single-thread compile -> sin contencion. */
    thread_local TypeChecker *g_active_typechecker = nullptr;

    /**
     * @brief implementacion del virtual fn `static_assert`
     * exportado bajo `extern "vesta_comptime"`.  Cuando un macro lo
     * invoca via la FFI dispatch (camino A), esta funcion recibe el
     * @c cond evaluado y un @c msg como C-string (host_ptr a bytes).
     *
     * Si @c cond es 0/falso, emite un diagnostic error en el TypeChecker
     * activo (g_active_typechecker) y returns 1 (status fail).  El AST
     * eval del macro recibe el u64 returned y puede propagar; pero el
     * mecanismo standar es: el diagnostic emite el error -> el macro
     * sigue ejecutando pero el compile final fallara con ese error.
     *
     * @c msg puede ser @c nullptr; en ese caso se usa un mensaje generic.
     *
     * @note Marcada @c extern "C" para que su nombre no sea mangled
     *       (asi @c register_virtual_fn la registra con simbolo limpio
     *       y la FFI dispatch la encuentra via fn_ptr).
     */
    /**
     * @brief virtual fn `comptime_compile(src) -> string`.
     *
     * Toma source Vex como C-string, lo trata como una EXPRESION Vex y
     * la devuelve TAL CUAL como string.  Sirve para componer codigo
     * desde macros sin que el AST evaluator se queje de "no es comptime
     * evaluable".  El AST resultante se parsea en el call site de la
     * macro como cualquier otro string de retorno @Macro.
     *
     * En v1 es esencialmente identity (devuelve src tal cual).  La
     * utilidad real esta en que el usuario puede pasar AST construido
     * via macros recursivos y obtener un string concreto sin
     * preocuparse de comptime_eval_expr.
     *
     * @c msg buffer compartido estatico para mantener el c_str() valido
     * mientras el caller lo necesite.  Limpio al destruir el
     * TypeChecker (cleanup via clear).
     */
    static std::string g_comptime_compile_buf;
    extern "C" const char *vex_comptime_compile(const char *src) {
        if (!src) return "";
        g_comptime_compile_buf = src;
        return g_comptime_compile_buf.c_str();
    }

    /**
     * @brief Phase MC.23: helpers virtual fn para queries de tipos por
     * NOMBRE (string).  Mucho mas simple que la version con hash: el
     * macro pasa "i32"/"u64"/"f32"/"<class_name>" y obtiene la metadata.
     *
     * Coverage: primitivos + clases + structs + enums.  Devuelve 0
     * si el nombre no se reconoce.
     */
    static Type type_from_name_str(const char *name) {
        if (!name) return Type{};
        const std::string nm{name};
        /* Primitivos. */
        if (nm == "i8")     return Type{PrimitiveKind::I8};
        if (nm == "i16")    return Type{PrimitiveKind::I16};
        if (nm == "i32")    return Type{PrimitiveKind::I32};
        if (nm == "i64")    return Type{PrimitiveKind::I64};
        if (nm == "u8")     return Type{PrimitiveKind::U8};
        if (nm == "u16")    return Type{PrimitiveKind::U16};
        if (nm == "u32")    return Type{PrimitiveKind::U32};
        if (nm == "u64")    return Type{PrimitiveKind::U64};
        if (nm == "f32")    return Type{PrimitiveKind::F32};
        if (nm == "f64")    return Type{PrimitiveKind::F64};
        if (nm == "bool")   return Type{PrimitiveKind::BOOL};
        if (nm == "char")   return Type{PrimitiveKind::CHAR};
        if (nm == "string") return Type{PrimitiveKind::STRING};
        if (nm == "ptr")    return Type{PrimitiveKind::PTR};
        /* Clase/struct/enum por nombre.  Buscamos en los layouts del
         * TypeChecker activo. */
        if (!g_active_typechecker) return Type{};
        const auto &cls = g_active_typechecker->class_layouts();
        if (cls.find(nm) != cls.end()) {
            Type t{PrimitiveKind::CLASS};
            t.struct_name = nm;
            return t;
        }
        const auto &str = g_active_typechecker->struct_layouts();
        if (str.find(nm) != str.end()) {
            Type t{PrimitiveKind::STRUCT};
            t.struct_name = nm;
            return t;
        }
        const auto &enm = g_active_typechecker->enum_layouts();
        if (enm.find(nm) != enm.end()) {
            Type t{PrimitiveKind::STRUCT};   // enum representado como STRUCT en Type
            t.struct_name = nm;
            return t;
        }
        return Type{};
    }

    extern "C" uint64_t vex_comptime_type_sizeof(const char *name) {
        if (!g_active_typechecker) return 0;
        const Type t = type_from_name_str(name);
        if (t.kind == PrimitiveKind::VOID) return 0;
        return comptime_type_size(*g_active_typechecker, t);
    }

    extern "C" uint64_t vex_comptime_type_alignof(const char *name) {
        if (!g_active_typechecker) return 0;
        const Type t = type_from_name_str(name);
        if (t.kind == PrimitiveKind::VOID) return 0;
        return comptime_type_align(*g_active_typechecker, t);
    }

    extern "C" uint64_t vex_comptime_type_kind(const char *name) {
        if (!g_active_typechecker) return 0;
        const Type t = type_from_name_str(name);
        return static_cast<uint64_t>(comptime_type_kind(t));
    }

    extern "C" uint64_t vex_static_assert(int64_t cond, const char *msg) {
        if (cond) return 0;   /* OK -- no-op. */
        const std::string text = msg ? std::string("static_assert: ") + msg
                                      : std::string("static_assert fallo");
        if (g_active_typechecker) {
            SourceLoc loc;
            loc.file = "<comptime>";
            g_active_typechecker->diagnostics().error(loc, text);
        } else {
            std::fprintf(stderr, "[vex] %s (sin TypeChecker activo)\n",
                          text.c_str());
        }
        return 1;             /* status fail (para el caller si lo lee). */
    }

    TypeChecker::TypeChecker(ast::ModuleNode &mod, Diagnostics &diags)
        : mod_(mod), diags_(diags) {
        // Reservar espacio razonable para evitar realocaciones en programas tipicos.
        scopes_.reserve(8);
        function_sigs_.reserve(16);
        /* marcar este TypeChecker como el activo + registrar
         * los virtual fns una vez por proceso (registration idempotent).
         * NOTA: g_active_typechecker se mantiene apuntando aqui hasta el
         * destructor.  Multi-instancia en paralelo no soportado todavia
         * (single-thread compile por diseno). */
        g_active_typechecker = this;
        static std::once_flag once_reg;
        std::call_once(once_reg, []() {
            ffi::register_virtual_fn(
                "vesta_comptime", "static_assert",
                reinterpret_cast<void *>(&vex_static_assert));
            /* type queries via virtual fns.  Macros invocan
             * `comptime_type_sizeof("i32")` etc. y obtienen metadata. */
            ffi::register_virtual_fn(
                "vesta_comptime", "comptime_type_sizeof",
                reinterpret_cast<void *>(&vex_comptime_type_sizeof));
            ffi::register_virtual_fn(
                "vesta_comptime", "comptime_type_alignof",
                reinterpret_cast<void *>(&vex_comptime_type_alignof));
            ffi::register_virtual_fn(
                "vesta_comptime", "comptime_type_kind",
                reinterpret_cast<void *>(&vex_comptime_type_kind));
            /* comptime_compile: identity en v1 (devuelve src tal cual).
             * Util para que el AST evaluator no rechace el call cuando
             * el macro hace `return comptime_compile(complicated_str)`. */
            ffi::register_virtual_fn(
                "vesta_comptime", "comptime_compile",
                reinterpret_cast<void *>(&vex_comptime_compile));
            /* Sprint B.1: thunk generator para callbacks Vex -> C nativos.
             * Builtin `as_native_callback(fn)` se baja a un CALLN a esta
             * fn que retorna el host_ptr al thunk callable con cc nativa.
             * El extern "C" decl global esta arriba del namespace. */
            ffi::register_virtual_fn(
                "vesta_runtime", "vex_get_native_thunk",
                reinterpret_cast<void *>(&::vex_get_native_thunk));
        });
    }

    TypeChecker::~TypeChecker() {
        /* Phase MC.20: limpiar el pointer thread_local SOLO si es esta
         * instancia (defensive: otro TypeChecker pudo haberse construido
         * y sobreescrito el slot). */
        if (g_active_typechecker == this) {
            g_active_typechecker = nullptr;
        }
    }

    // =====================================================================
    //  Generics: AST cloning + type substitution
    //
    //  La monomorphizacion clona el AST de la clase generica sustituyendo
    //  los type params (T, U, ...) por los args concretos en TODOS los
    //  TypeNodes encontrados (firmas, bodies de metodo, etc).  Despues la
    //  clase clonada se procesa por collect_classes / check_functions
    //  como una clase normal.  El cloning se hace una vez por
    //  (template, args) y se cachea en monomorphized_.
    // =====================================================================

    /// Mapping nombre-de-param -> tipo concreto (usado para substituir
    /// NamedTypeNode("T") por el correspondiente en la instanciacion).
    /// Para implementarlo simple lo guardamos como un array paralelo.
    struct GenSubst {
        const std::vector<std::string> *params;
        const std::vector<Type>        *args;
    };

    static std::string mangle_type(const Type &t);

    static std::string mangle_args(const std::vector<Type> &args) {
        std::string s;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += "_";
            s += mangle_type(args[i]);
        }
        return s;
    }

    static std::string mangle_type(const Type &t) {
        switch (t.kind) {
            case PrimitiveKind::I8:   return "i8";
            case PrimitiveKind::I16:  return "i16";
            case PrimitiveKind::I32:  return "i32";
            case PrimitiveKind::I64:  return "i64";
            case PrimitiveKind::U8:   return "u8";
            case PrimitiveKind::U16:  return "u16";
            case PrimitiveKind::U32:  return "u32";
            case PrimitiveKind::U64:  return "u64";
            case PrimitiveKind::F32:  return "f32";
            case PrimitiveKind::F64:  return "f64";
            case PrimitiveKind::BOOL: return "bool";
            case PrimitiveKind::CHAR: return "ch";
            case PrimitiveKind::PTR:  return "ptr";
            case PrimitiveKind::CLASS: case PrimitiveKind::STRUCT:
                return t.struct_name;
            default: return "x";
        }
    }

    static std::unique_ptr<ast::TypeNode> clone_type_with_subst(
        const ast::TypeNode *t, const GenSubst &g);
    static std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e,
                                                  const GenSubst &g = {});
    static std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s,
                                                  const GenSubst &g = {});

    static std::unique_ptr<ast::TypeNode> clone_type_with_subst(
        const ast::TypeNode *t, const GenSubst &g)
    {
        if (!t) return nullptr;
        switch (t->kind) {
            case ast::NodeKind::PrimitiveTypeNode: {
                auto *src = static_cast<const ast::PrimitiveTypeNode *>(t);
                auto p = std::make_unique<ast::PrimitiveTypeNode>();
                p->loc  = src->loc;
                p->prim = src->prim;
                // tipo args para colecciones genericas
                // (ArrayList<T>) deben replicarse al clonar el AST por
                // monomorphizacion u otras transformaciones.
                for (auto &ta : src->type_args) {
                    p->type_args.push_back(clone_type_with_subst(ta.get(), g));
                }
                return p;
            }
            case ast::NodeKind::NamedTypeNode: {
                auto *src = static_cast<const ast::NamedTypeNode *>(t);
                // Si el nombre es uno de los type params, sustituimos por
                // el tipo concreto del binding.
                if (g.params && g.args) {
                    for (size_t i = 0; i < g.params->size(); ++i) {
                        if ((*g.params)[i] == src->name) {
                            const Type &a = (*g.args)[i];
                            // Si el arg es primitivo, generamos un
                            // PrimitiveTypeNode.  Si es CLASS/STRUCT,
                            // generamos un NamedTypeNode con el nombre
                            // de la clase concreta.
                            if (a.kind != PrimitiveKind::CLASS
                             && a.kind != PrimitiveKind::STRUCT) {
                                auto p = std::make_unique<ast::PrimitiveTypeNode>();
                                p->loc  = src->loc;
                                p->prim = a.kind;
                                return p;
                            }
                            auto n = std::make_unique<ast::NamedTypeNode>();
                            n->loc  = src->loc;
                            n->name = a.struct_name;
                            return n;
                        }
                    }
                }
                auto n = std::make_unique<ast::NamedTypeNode>();
                n->loc  = src->loc;
                n->name = src->name;
                for (auto &ta : src->type_args) {
                    n->type_args.push_back(clone_type_with_subst(ta.get(), g));
                }
                return n;
            }
            case ast::NodeKind::PointerTypeNode: {
                auto *src = static_cast<const ast::PointerTypeNode *>(t);
                auto p = std::make_unique<ast::PointerTypeNode>();
                p->loc     = src->loc;
                p->pointee = clone_type_with_subst(src->pointee.get(), g);
                return p;
            }
            case ast::NodeKind::ArrayTypeNode: {
                auto *src = static_cast<const ast::ArrayTypeNode *>(t);
                auto a = std::make_unique<ast::ArrayTypeNode>();
                a->loc          = src->loc;
                a->element_type = clone_type_with_subst(src->element_type.get(), g);
                if (src->size_expr) a->size_expr = clone_expr(src->size_expr.get(), g);
                return a;
            }
            default: return nullptr;
        }
    }

    static std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e,
                                                  const GenSubst &g) {
        if (!e) return nullptr;
        switch (e->kind) {
            case ast::NodeKind::IntLitExpr: {
                auto *s = static_cast<const ast::IntLitExpr *>(e);
                auto x = std::make_unique<ast::IntLitExpr>();
                x->loc = s->loc; x->value = s->value;
                return x;
            }
            case ast::NodeKind::FloatLitExpr: {
                auto *s = static_cast<const ast::FloatLitExpr *>(e);
                auto x = std::make_unique<ast::FloatLitExpr>();
                x->loc = s->loc; x->value = s->value;
                return x;
            }
            case ast::NodeKind::BoolLitExpr: {
                auto *s = static_cast<const ast::BoolLitExpr *>(e);
                auto x = std::make_unique<ast::BoolLitExpr>();
                x->loc = s->loc; x->value = s->value;
                return x;
            }
            case ast::NodeKind::NullLitExpr: {
                auto x = std::make_unique<ast::NullLitExpr>();
                x->loc = e->loc;
                return x;
            }
            case ast::NodeKind::CharLitExpr: {
                auto *s = static_cast<const ast::CharLitExpr *>(e);
                auto x = std::make_unique<ast::CharLitExpr>();
                x->loc = s->loc; x->codepoint = s->codepoint;
                return x;
            }
            case ast::NodeKind::StringLitExpr: {
                auto *s = static_cast<const ast::StringLitExpr *>(e);
                auto x = std::make_unique<ast::StringLitExpr>();
                x->loc = s->loc; x->value = s->value;
                return x;
            }
            case ast::NodeKind::IdentExpr: {
                auto *s = static_cast<const ast::IdentExpr *>(e);
                auto x = std::make_unique<ast::IdentExpr>();
                x->loc = s->loc; x->name = s->name;
                return x;
            }
            case ast::NodeKind::ThisExpr: {
                auto x = std::make_unique<ast::ThisExpr>();
                x->loc = e->loc;
                return x;
            }
            case ast::NodeKind::FieldAccessExpr: {
                auto *s = static_cast<const ast::FieldAccessExpr *>(e);
                auto x = std::make_unique<ast::FieldAccessExpr>();
                x->loc        = s->loc;
                x->base       = clone_expr(s->base.get(), g);
                x->field_name = s->field_name;
                return x;
            }
            case ast::NodeKind::BinaryExpr: {
                auto *s = static_cast<const ast::BinaryExpr *>(e);
                auto x = std::make_unique<ast::BinaryExpr>();
                x->loc = s->loc; x->op = s->op;
                x->lhs = clone_expr(s->lhs.get(), g);
                x->rhs = clone_expr(s->rhs.get(), g);
                return x;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *s = static_cast<const ast::UnaryExpr *>(e);
                auto x = std::make_unique<ast::UnaryExpr>();
                x->loc = s->loc; x->op = s->op;
                x->operand = clone_expr(s->operand.get(), g);
                return x;
            }
            case ast::NodeKind::CastExpr: {
                auto *s = static_cast<const ast::CastExpr *>(e);
                auto x = std::make_unique<ast::CastExpr>();
                x->loc         = s->loc;
                x->target_type = clone_type_with_subst(s->target_type.get(), g);
                x->operand     = clone_expr(s->operand.get(), g);
                return x;
            }
            case ast::NodeKind::AssignExpr: {
                auto *s = static_cast<const ast::AssignExpr *>(e);
                auto x = std::make_unique<ast::AssignExpr>();
                x->loc = s->loc; x->op = s->op;
                x->target = clone_expr(s->target.get(), g);
                x->value  = clone_expr(s->value.get(), g);
                return x;
            }
            case ast::NodeKind::CallExpr: {
                auto *s = static_cast<const ast::CallExpr *>(e);
                auto x = std::make_unique<ast::CallExpr>();
                x->loc    = s->loc;
                x->callee = clone_expr(s->callee.get(), g);
                for (auto &a : s->args) x->args.push_back(clone_expr(a.get(), g));
                return x;
            }
            case ast::NodeKind::IndexExpr: {
                auto *s = static_cast<const ast::IndexExpr *>(e);
                auto x = std::make_unique<ast::IndexExpr>();
                x->loc   = s->loc;
                x->base  = clone_expr(s->base.get(), g);
                x->index = clone_expr(s->index.get(), g);
                return x;
            }
            case ast::NodeKind::NewExpr: {
                auto *s = static_cast<const ast::NewExpr *>(e);
                auto x = std::make_unique<ast::NewExpr>();
                x->loc        = s->loc;
                // BugFix P1-A1: si class_name matchea un type-param,
                // substituir al tipo concreto (e.g. `new T[cap]` -> `new i32[cap]`).
                // Sin esto el monomorphized clone tenia `new T[cap]` literal y
                // el type checker reportaba "tipo desconocido 'T'".  Si el
                // arg es CLASS/STRUCT/ENUM, usa el struct_name.  Si es
                // primitivo, usa el nombre canonico ("i32", "f64", etc).
                bool substituted = false;
                if (g.params && g.args) {
                    for (size_t i = 0; i < g.params->size(); ++i) {
                        if ((*g.params)[i] == s->class_name) {
                            const Type &a = (*g.args)[i];
                            if (a.kind == PrimitiveKind::CLASS
                             || a.kind == PrimitiveKind::STRUCT) {
                                x->class_name = a.struct_name;
                            } else {
                                x->class_name = type_to_string(a);
                            }
                            substituted = true;
                            break;
                        }
                    }
                }
                if (!substituted) x->class_name = s->class_name;
                for (auto &a : s->args)      x->args.push_back(clone_expr(a.get(), g));
                for (auto &t : s->type_args) x->type_args.push_back(
                    clone_type_with_subst(t.get(), g));
                if (s->array_size) x->array_size = clone_expr(s->array_size.get(), g);
                return x;
            }
            case ast::NodeKind::TernaryExpr: {
                auto *s = static_cast<const ast::TernaryExpr *>(e);
                auto x = std::make_unique<ast::TernaryExpr>();
                x->loc       = s->loc;
                x->cond      = clone_expr(s->cond.get(), g);
                x->then_expr = clone_expr(s->then_expr.get(), g);
                x->else_expr = clone_expr(s->else_expr.get(), g);
                return x;
            }
            case ast::NodeKind::TryExpr: {
                auto *s = static_cast<const ast::TryExpr *>(e);
                auto x = std::make_unique<ast::TryExpr>();
                x->loc     = s->loc;
                x->operand = clone_expr(s->operand.get(), g);
                return x;
            }
            case ast::NodeKind::MatchExpr: {
                auto *s = static_cast<const ast::MatchExpr *>(e);
                auto x = std::make_unique<ast::MatchExpr>();
                x->loc       = s->loc;
                x->scrutinee = clone_expr(s->scrutinee.get(), g);
                for (const auto &arm : s->arms) {
                    ast::MatchArm na;
                    na.loc          = arm.loc;
                    na.variant_name = arm.variant_name;
                    na.bindings     = arm.bindings;
                    if (arm.body) na.body = clone_stmt(arm.body.get(), g);
                    if (arm.guard) na.guard = clone_expr(arm.guard.get(), g);
                    x->arms.push_back(std::move(na));
                }
                return x;
            }
            case ast::NodeKind::InitListExpr: {
                auto *s = static_cast<const ast::InitListExpr *>(e);
                auto x = std::make_unique<ast::InitListExpr>();
                x->loc              = s->loc;
                x->is_designated    = s->is_designated;
                x->field_names      = s->field_names;
                x->target_type_name = s->target_type_name;
                for (auto &el : s->elements) x->elements.push_back(clone_expr(el.get(), g));
                return x;
            }
            case ast::NodeKind::LambdaExpr: {
                auto *s = static_cast<const ast::LambdaExpr *>(e);
                auto x = std::make_unique<ast::LambdaExpr>();
                x->loc = s->loc;
                for (const auto &p : s->params) {
                    auto np  = std::make_unique<ast::ParamDecl>();
                    np->loc  = p->loc;
                    np->name = p->name;
                    np->type = clone_type_with_subst(p->type.get(), g);
                    x->params.push_back(std::move(np));
                }
                if (s->return_type)
                    x->return_type = clone_type_with_subst(s->return_type.get(), g);
                if (s->body) {
                    auto cb = clone_stmt(s->body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                return x;
            }
            case ast::NodeKind::SpawnExpr: {
                auto *s = static_cast<const ast::SpawnExpr *>(e);
                auto x = std::make_unique<ast::SpawnExpr>();
                x->loc    = s->loc;
                x->policy = s->policy;
                if (s->sched_idx) x->sched_idx = clone_expr(s->sched_idx.get(), g);
                if (s->body) {
                    auto cb = clone_stmt(s->body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                return x;
            }
            case ast::NodeKind::RSpawnExpr: {
                auto *s = static_cast<const ast::RSpawnExpr *>(e);
                auto x = std::make_unique<ast::RSpawnExpr>();
                x->loc      = s->loc;
                x->node_idx = clone_expr(s->node_idx.get(), g);
                if (s->body) {
                    auto cb = clone_stmt(s->body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                return x;
            }
            default: return nullptr;
        }
    }

    static std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s,
                                                  const GenSubst &g) {
        if (!s) return nullptr;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                auto *src = static_cast<const ast::BlockStmt *>(s);
                auto x = std::make_unique<ast::BlockStmt>();
                x->loc = src->loc;
                for (auto &b : src->body) x->body.push_back(clone_stmt(b.get(), g));
                return x;
            }
            case ast::NodeKind::VarDeclStmt: {
                auto *src = static_cast<const ast::VarDeclStmt *>(s);
                auto x = std::make_unique<ast::VarDeclStmt>();
                x->loc      = src->loc;
                x->name     = src->name;
                x->is_const = src->is_const;
                x->type     = clone_type_with_subst(src->type.get(), g);
                if (src->init) x->init = clone_expr(src->init.get(), g);
                return x;
            }
            case ast::NodeKind::ExprStmt: {
                auto *src = static_cast<const ast::ExprStmt *>(s);
                auto x = std::make_unique<ast::ExprStmt>();
                x->loc  = src->loc;
                x->expr = clone_expr(src->expr.get(), g);
                return x;
            }
            case ast::NodeKind::IfStmt: {
                auto *src = static_cast<const ast::IfStmt *>(s);
                auto x = std::make_unique<ast::IfStmt>();
                x->loc         = src->loc;
                x->cond        = clone_expr(src->cond.get(), g);
                x->then_branch = clone_stmt(src->then_branch.get(), g);
                x->else_branch = clone_stmt(src->else_branch.get(), g);
                return x;
            }
            case ast::NodeKind::WhileStmt: {
                auto *src = static_cast<const ast::WhileStmt *>(s);
                auto x = std::make_unique<ast::WhileStmt>();
                x->loc  = src->loc;
                x->cond = clone_expr(src->cond.get(), g);
                x->body = clone_stmt(src->body.get(), g);
                return x;
            }
            case ast::NodeKind::DoWhileStmt: {
                auto *src = static_cast<const ast::DoWhileStmt *>(s);
                auto x = std::make_unique<ast::DoWhileStmt>();
                x->loc  = src->loc;
                x->body = clone_stmt(src->body.get(), g);
                x->cond = clone_expr(src->cond.get(), g);
                return x;
            }
            case ast::NodeKind::ForStmt: {
                auto *src = static_cast<const ast::ForStmt *>(s);
                auto x = std::make_unique<ast::ForStmt>();
                x->loc  = src->loc;
                x->init = clone_stmt(src->init.get(), g);
                x->cond = clone_expr(src->cond.get(), g);
                x->step = clone_expr(src->step.get(), g);
                x->body = clone_stmt(src->body.get(), g);
                return x;
            }
            case ast::NodeKind::ReturnStmt: {
                auto *src = static_cast<const ast::ReturnStmt *>(s);
                auto x = std::make_unique<ast::ReturnStmt>();
                x->loc   = src->loc;
                if (src->value) x->value = clone_expr(src->value.get(), g);
                return x;
            }
            case ast::NodeKind::BreakStmt: {
                auto x = std::make_unique<ast::BreakStmt>();
                x->loc = s->loc;
                return x;
            }
            case ast::NodeKind::ContinueStmt: {
                auto x = std::make_unique<ast::ContinueStmt>();
                x->loc = s->loc;
                return x;
            }
            case ast::NodeKind::ThrowStmt: {
                auto *src = static_cast<const ast::ThrowStmt *>(s);
                auto x = std::make_unique<ast::ThrowStmt>();
                x->loc   = src->loc;
                x->value = clone_expr(src->value.get(), g);
                return x;
            }
            case ast::NodeKind::TryStmt: {
                auto *src = static_cast<const ast::TryStmt *>(s);
                auto x = std::make_unique<ast::TryStmt>();
                x->loc = src->loc;
                if (src->body) {
                    auto cb = clone_stmt(src->body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                for (const auto &c : src->catches) {
                    ast::CatchClause nc;
                    nc.loc            = c.loc;
                    nc.exc_class_name = c.exc_class_name;
                    nc.var_name       = c.var_name;
                    if (c.body) {
                        auto cb = clone_stmt(c.body.get(), g);
                        if (cb && cb->kind == ast::NodeKind::BlockStmt)
                            nc.body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                    }
                    x->catches.push_back(std::move(nc));
                }
                if (src->finally_body) {
                    auto cb = clone_stmt(src->finally_body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->finally_body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                return x;
            }
            case ast::NodeKind::ForEachStmt: {
                auto *src = static_cast<const ast::ForEachStmt *>(s);
                auto x = std::make_unique<ast::ForEachStmt>();
                x->loc       = src->loc;
                x->iter_name = src->iter_name;
                x->iter_type = clone_type_with_subst(src->iter_type.get(), g);
                x->iter_expr = clone_expr(src->iter_expr.get(), g);
                x->body      = clone_stmt(src->body.get(), g);
                return x;
            }
            case ast::NodeKind::SynchronizedStmt: {
                auto *src = static_cast<const ast::SynchronizedStmt *>(s);
                auto x = std::make_unique<ast::SynchronizedStmt>();
                x->loc    = src->loc;
                x->target = clone_expr(src->target.get(), g);
                if (src->body) {
                    auto cb = clone_stmt(src->body.get(), g);
                    if (cb && cb->kind == ast::NodeKind::BlockStmt)
                        x->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
                return x;
            }
            case ast::NodeKind::GotoStmt: {
                auto *src = static_cast<const ast::GotoStmt *>(s);
                auto x = std::make_unique<ast::GotoStmt>();
                x->loc   = src->loc;
                x->label = src->label;
                return x;
            }
            case ast::NodeKind::LabelStmt: {
                auto *src = static_cast<const ast::LabelStmt *>(s);
                auto x = std::make_unique<ast::LabelStmt>();
                x->loc  = src->loc;
                x->name = src->name;
                return x;
            }
            case ast::NodeKind::ComptimeBlockStmt: {
                auto *src = static_cast<const ast::ComptimeBlockStmt *>(s);
                auto x = std::make_unique<ast::ComptimeBlockStmt>();
                x->loc = src->loc;
                for (auto &b : src->stmts) x->stmts.push_back(clone_stmt(b.get(), g));
                return x;
            }
            case ast::NodeKind::ComptimeForStmt: {
                auto *src = static_cast<const ast::ComptimeForStmt *>(s);
                auto x = std::make_unique<ast::ComptimeForStmt>();
                x->loc       = src->loc;
                x->var_name  = src->var_name;
                x->inclusive = src->inclusive;
                x->lo_expr   = clone_expr(src->lo_expr.get(), g);
                x->hi_expr   = clone_expr(src->hi_expr.get(), g);
                x->body      = clone_stmt(src->body.get(), g);
                return x;
            }
            default: return nullptr;
        }
    }

    std::string TypeChecker::monomorphize_class(const std::string &template_name,
                                                  const std::vector<Type> &args,
                                                  const SourceLoc &loc) {
        const std::string mangled = template_name + "_" + mangle_args(args);
        if (monomorphized_.count(mangled)) return mangled;

        auto it = generic_templates_.find(template_name);
        if (it == generic_templates_.end()) {
            diags_.error(loc,
                "tipo generico desconocido: '" + template_name + "'");
            return std::string();
        }
        auto *tmpl = static_cast<const ast::ClassDecl *>(
            mod_.decls[it->second].get());
        if (tmpl->type_params.size() != args.size()) {
            diags_.error(loc,
                "numero incorrecto de args de tipo para '" + template_name +
                "': esperados " + std::to_string(tmpl->type_params.size()) +
                ", recibidos " + std::to_string(args.size()));
            return std::string();
        }

        GenSubst g{&tmpl->type_params, &args};

        auto cloned = std::make_unique<ast::ClassDecl>();
        cloned->loc            = tmpl->loc;
        cloned->name           = mangled;
        cloned->super_name     = tmpl->super_name;
        cloned->interface_names = tmpl->interface_names;
        cloned->is_final       = tmpl->is_final;
        cloned->is_aspect      = tmpl->is_aspect;
        cloned->is_interface   = tmpl->is_interface;
        cloned->is_introspect  = tmpl->is_introspect;
        // type_params vacio: ya es concreto.

        // Clonar fields.
        for (const auto &f : tmpl->fields) {
            ast::ClassFieldDecl nf;
            nf.loc       = f.loc;
            nf.name      = f.name;
            nf.access    = f.access;
            nf.is_static = f.is_static;
            nf.is_final  = f.is_final;
            nf.type      = clone_type_with_subst(f.type.get(), g);
            if (f.init) nf.init = clone_expr(f.init.get(), g);
            cloned->fields.push_back(std::move(nf));
        }
        // Clonar metodos.
        for (const auto &m : tmpl->methods) {
            auto nm = std::make_unique<ast::ClassMethodDecl>();
            nm->loc            = m->loc;
            // Si el metodo es el constructor del template, su nombre
            // textualmente coincide con el template_name; en la version
            // monomorphizada debe coincidir con el mangled name.
            nm->name           = m->is_constructor ? mangled : m->name;
            nm->access         = m->access;
            nm->is_static      = m->is_static;
            nm->is_final       = m->is_final;
            nm->is_override    = m->is_override;
            nm->is_constructor = m->is_constructor;
            nm->advice_kind    = m->advice_kind;
            nm->advice_target  = m->advice_target;
            if (m->return_type) {
                nm->return_type = clone_type_with_subst(m->return_type.get(), g);
            }
            for (const auto &p : m->params) {
                auto np = std::make_unique<ast::ParamDecl>();
                np->loc  = p->loc;
                np->name = p->name;
                np->type = clone_type_with_subst(p->type.get(), g);
                nm->params.push_back(std::move(np));
            }
            if (m->body) {
                auto cb = clone_stmt(m->body.get(), g);
                if (cb && cb->kind == ast::NodeKind::BlockStmt) {
                    nm->body.reset(static_cast<ast::BlockStmt *>(cb.release()));
                }
            }
            cloned->methods.push_back(std::move(nm));
        }

        monomorphized_[mangled] = true;

        // B.3 contract: registrar provenance para que el lowering
        // marque cada IrFunction generada con template_name + type_args
        // legibles.  Los nombres legibles vienen de @c type_to_string
        // sobre cada arg (e.g., "i32", "string", "Box<i64>" para
        // generics anidados).
        MonomorphInfo info;
        info.template_name = template_name;
        info.type_args.reserve(args.size());
        for (const auto &t : args) {
            info.type_args.push_back(type_to_string(t));
        }
        monomorph_info_[mangled] = std::move(info);

        // BugFix P1-A4: pre-registrar la clase monomorphizada en
        // class_layouts_ con layout vacio.  Sin esto, type_from_node de
        // generics anidados (`Container<Pair<i32,i64>>`) devolvia Type{}
        // porque Pair_i32_i64 aun no estaba en class_layouts_ cuando el
        // outer se monomorphizaba.  El pre-pase de class_layouts (en
        // collect_globals) sobrescribe esta entrada con el layout real.
        if (!class_layouts_.count(mangled)) {
            ClassLayout empty;
            empty.name = mangled;
            class_layouts_[mangled] = std::move(empty);
        }

        mod_.decls.push_back(std::move(cloned));
        return mangled;
    }


    // L2.3: monomorphize_enum -- equivalente a monomorphize_class pero
    // sobre EnumDecl.  Crea una copia concreta del template con los
    // type_params sustituidos por args concretos, registra la entrada en
    // generic_templates_-stylee y la anade a mod_.decls para que el pase
    // 2 de collect_globals la procese como enum normal y genere su
    // EnumLayout.  Idempotente: si la version (template, args) ya esta
    // monomorphizada, retorna el mangled name sin clonar.
    std::string TypeChecker::monomorphize_enum(const std::string &template_name,
                                                 const std::vector<Type> &args,
                                                 const SourceLoc &loc) {
        const std::string mangled = template_name + "_" + mangle_args(args);
        if (monomorphized_.count(mangled)) return mangled;

        auto it = generic_enum_templates_.find(template_name);
        if (it == generic_enum_templates_.end()) {
            diags_.error(loc,
                "enum generico desconocido: '" + template_name + "'");
            return std::string();
        }
        auto *tmpl = static_cast<const ast::EnumDecl *>(
            mod_.decls[it->second].get());
        if (tmpl->type_params.size() != args.size()) {
            diags_.error(loc,
                "numero incorrecto de args de tipo para enum '" + template_name +
                "': esperados " + std::to_string(tmpl->type_params.size()) +
                ", recibidos " + std::to_string(args.size()));
            return std::string();
        }

        GenSubst g{&tmpl->type_params, &args};

        auto cloned = std::make_unique<ast::EnumDecl>();
        cloned->loc          = tmpl->loc;
        cloned->name         = mangled;
        cloned->is_introspect = tmpl->is_introspect;
        cloned->is_public    = tmpl->is_public;
        // type_params vacio: ya es concreto.

        // Clonar variantes sustituyendo los payload types.
        for (const auto &v : tmpl->variants) {
            ast::EnumVariantDecl nv;
            nv.loc  = v.loc;
            nv.name = v.name;
            nv.field_types.reserve(v.field_types.size());
            for (const auto &ft : v.field_types) {
                nv.field_types.push_back(clone_type_with_subst(ft.get(), g));
            }
            cloned->variants.push_back(std::move(nv));
        }

        monomorphized_[mangled] = true;

        // Construir el EnumLayout concreto INMEDIATAMENTE (no diferimos al
        // pase 2 porque el monomorphize_enum puede invocarse on-demand
        // durante check_call cuando ya estamos pasado el pase 2).
        EnumLayout elay;
        elay.name = mangled;
        elay.is_introspect = tmpl->is_introspect;
        elay.is_public = tmpl->is_public;
        uint32_t max_pl = 0;
        for (size_t vi = 0; vi < cloned->variants.size(); ++vi) {
            const auto &vd = cloned->variants[vi];
            EnumVariantInfo vi_info;
            vi_info.name = vd.name;
            vi_info.tag  = static_cast<uint32_t>(vi);
            vi_info.field_types.reserve(vd.field_types.size());
            for (const auto &ft : vd.field_types) {
                vi_info.field_types.push_back(type_from_node(ft.get()));
            }
            if (vi_info.field_types.size() > max_pl) {
                max_pl = static_cast<uint32_t>(vi_info.field_types.size());
            }
            elay.variants.push_back(std::move(vi_info));
        }
        elay.max_payload_fields = max_pl;
        elay.size_bytes = 8 + 8 * max_pl;
        enum_layouts_[mangled] = std::move(elay);

        mod_.decls.push_back(std::move(cloned));
        return mangled;
    }


    bool TypeChecker::class_is_assignable(const Type &target,
                                            const Type &value) const noexcept {
        // Optional/Result: dos Optional<X> son asignables si X y Y
        // son ambos numericos (coercion implicita comun: Some(50) tipa
        // como Optional<i64> y se asigna a Optional<i32> sin error,
        // igual que la regla numerica para escalares).  Tambien para
        // Result<V1,E1> -> Result<V2,E2>.
        if (target.kind == PrimitiveKind::OPTIONAL
         && value.kind  == PrimitiveKind::OPTIONAL
         && target.pointee && value.pointee
         && is_numeric(target.pointee->kind)
         && is_numeric(value.pointee->kind)) {
            return true;
        }
        if (target.kind == PrimitiveKind::RESULT
         && value.kind  == PrimitiveKind::RESULT
         && target.pointee && value.pointee
         && target.pointee2 && value.pointee2
         && is_numeric(target.pointee->kind)  && is_numeric(value.pointee->kind)
         && is_numeric(target.pointee2->kind) && is_numeric(value.pointee2->kind)) {
            return true;
        }
        if (target.kind != PrimitiveKind::CLASS) return false;
        if (value.kind  != PrimitiveKind::CLASS) return false;
        if (target.struct_name == value.struct_name) return true;
        // Buscar la cadena de supers/interfaces de @c value.
        std::string current = value.struct_name;
        // Cota dura para evitar bucles si la jerarquia esta corrupta.
        for (int depth = 0; depth < 256; ++depth) {
            auto it = class_layouts_.find(current);
            if (it == class_layouts_.end()) return false;
            const ClassLayout &cl = it->second;
            // Las interfaces declaradas (incluyendo super-interfaz si la
            // hubiese) se consideran tipos asignables del receptor.
            for (const std::string &iname : cl.interface_names) {
                if (iname == target.struct_name) return true;
                // Permitimos transitividad: si la interfaz extiende otra,
                // el target podria ser la super-interfaz.  Cota: 1 nivel
                // explicito; recursion completa requeriria un BFS aparte.
                auto it_i = class_layouts_.find(iname);
                if (it_i != class_layouts_.end()) {
                    for (const std::string &super_i : it_i->second.interface_names) {
                        if (super_i == target.struct_name) return true;
                    }
                }
            }
            if (cl.super_name.empty()) return false;
            if (cl.super_name == target.struct_name) return true;
            current = cl.super_name;
        }
        return false;
    }

    // ----- Pre-pase de monomorphizacion generics) ---------------
    //
    // Recorre el AST detectando referencias a clases con type_args (en
    // tipos de var-decl, params, returns, fields, news) y genera los
    // ClassDecls concretos por monomorphizacion antes de que
    // collect_globals los necesite.  Tambien identifica los templates y
    // los registra en generic_templates_ para skip-check.
    //
    // El recorrido es tolerante: si encuentra un tipo cuyo template no
    // existe (puede ser una clase concreta no-generica), simplemente lo
    // ignora.  Errores se reportan despues por type_from_node.
    // ----------------------------------------------------------------
    static void pre_mono_collect_in_type(TypeChecker &tc,
                                          const ast::TypeNode *t,
                                          const SourceLoc &loc);
    static void pre_mono_collect_in_expr(TypeChecker &tc,
                                          const ast::Expr *e);
    static void pre_mono_collect_in_stmt(TypeChecker &tc,
                                          const ast::Stmt *s);

    static void pre_mono_collect_in_type(TypeChecker &tc,
                                          const ast::TypeNode *t,
                                          const SourceLoc &loc) {
        if (!t) return;
        if (t->kind == ast::NodeKind::NamedTypeNode) {
            const auto *nt = static_cast<const ast::NamedTypeNode *>(t);
            // Optional<T> y Result<V,E> son builtins del compilador,
            // NO templates de usuario: no se monomorphizan.  Se ignoran
            // aqui; type_from_node los resuelve a Type{OPTIONAL/RESULT}.
            // Pero SI necesitamos recursar en los args para detectar
            // templates de usuario anidados (Optional<MyTpl<i32>>).
            if (nt->name == "Optional" || nt->name == "Result"
             || nt->name == "Future") {
                // Mejora II: Future<T> es builtin igual que Optional/Result.
                for (auto &ta : nt->type_args) {
                    pre_mono_collect_in_type(tc, ta.get(), loc);
                }
                return;
            }
            if (!nt->type_args.empty()) {
                // BugFix P1-A4: monomorphizar args ANIDADOS primero, para
                // que cuando resolvamos el outer (`Container<Pair<i32,i64>>`),
                // el Type::struct_name del inner ya sea "Pair_i32_i64".
                // Sin esto, resolve_type_node devolvia COUNT para Pair<...>
                // y la mangle quedaba mal.
                for (auto &ta : nt->type_args) {
                    pre_mono_collect_in_type(tc, ta.get(), loc);
                }
                std::vector<Type> args;
                args.reserve(nt->type_args.size());
                for (auto &ta : nt->type_args) {
                    args.push_back(tc.resolve_type_node(ta.get()));
                }
                // L2.3: si es enum generico, monomorphizar como enum.
                if (tc.is_generic_enum_template(nt->name)) {
                    (void)tc.monomorphize_enum(nt->name, args, loc);
                } else {
                    (void)tc.monomorphize_class(nt->name, args, loc);
                }
            }
        } else if (t->kind == ast::NodeKind::PointerTypeNode) {
            const auto *pn = static_cast<const ast::PointerTypeNode *>(t);
            pre_mono_collect_in_type(tc, pn->pointee.get(), loc);
        } else if (t->kind == ast::NodeKind::ArrayTypeNode) {
            const auto *an = static_cast<const ast::ArrayTypeNode *>(t);
            pre_mono_collect_in_type(tc, an->element_type.get(), loc);
        }
    }

    static void pre_mono_collect_in_expr(TypeChecker &tc,
                                          const ast::Expr *e) {
        if (!e) return;
        switch (e->kind) {
            case ast::NodeKind::NewExpr: {
                auto *ne = static_cast<const ast::NewExpr *>(e);
                // BugFix R5: Optional/Result/Future son builtins NO
                // templates de usuario.  Skipear monomorphize_class para
                // ellos; check_new tiene su propio handling.  Recursamos
                // en type_args para detectar templates anidados.
                if (ne->class_name == "Optional"
                 || ne->class_name == "Result"
                 || ne->class_name == "Future") {
                    for (auto &ta : ne->type_args) {
                        pre_mono_collect_in_type(tc, ta.get(), ne->loc);
                    }
                    for (auto &a : ne->args) pre_mono_collect_in_expr(tc, a.get());
                    return;
                }
                if (!ne->type_args.empty()) {
                    // BugFix P1-A4: monomorphizar args ANIDADOS primero,
                    // identical fix que para NamedTypeNode.  Sin esto
                    // `new Container<Pair<i32,i64>>(...)` no monomorphiza
                    // Container_Pair_i32_i64 porque resolve_type_node de
                    // Pair<...> devuelve COUNT.
                    for (auto &ta : ne->type_args) {
                        pre_mono_collect_in_type(tc, ta.get(), ne->loc);
                    }
                    std::vector<Type> args;
                    args.reserve(ne->type_args.size());
                    for (auto &ta : ne->type_args) {
                        args.push_back(tc.resolve_type_node(ta.get()));
                    }
                    (void)tc.monomorphize_class(ne->class_name, args, ne->loc);
                }
                for (auto &a : ne->args) pre_mono_collect_in_expr(tc, a.get());
                return;
            }
            case ast::NodeKind::BinaryExpr: {
                auto *b = static_cast<const ast::BinaryExpr *>(e);
                pre_mono_collect_in_expr(tc, b->lhs.get());
                pre_mono_collect_in_expr(tc, b->rhs.get());
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *u = static_cast<const ast::UnaryExpr *>(e);
                pre_mono_collect_in_expr(tc, u->operand.get());
                return;
            }
            case ast::NodeKind::CastExpr: {
                auto *c = static_cast<const ast::CastExpr *>(e);
                if (c->target_type) {
                    pre_mono_collect_in_type(tc, c->target_type.get(), c->loc);
                }
                pre_mono_collect_in_expr(tc, c->operand.get());
                return;
            }
            case ast::NodeKind::AssignExpr: {
                auto *a = static_cast<const ast::AssignExpr *>(e);
                pre_mono_collect_in_expr(tc, a->target.get());
                pre_mono_collect_in_expr(tc, a->value.get());
                return;
            }
            case ast::NodeKind::CallExpr: {
                auto *c = static_cast<const ast::CallExpr *>(e);
                pre_mono_collect_in_expr(tc, c->callee.get());
                for (auto &a : c->args) pre_mono_collect_in_expr(tc, a.get());
                return;
            }
            case ast::NodeKind::FieldAccessExpr: {
                auto *f = static_cast<const ast::FieldAccessExpr *>(e);
                pre_mono_collect_in_expr(tc, f->base.get());
                return;
            }
            case ast::NodeKind::IndexExpr: {
                auto *i = static_cast<const ast::IndexExpr *>(e);
                pre_mono_collect_in_expr(tc, i->base.get());
                pre_mono_collect_in_expr(tc, i->index.get());
                return;
            }
            default: return;
        }
    }

    static void pre_mono_collect_in_stmt(TypeChecker &tc,
                                          const ast::Stmt *s) {
        if (!s) return;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                auto *b = static_cast<const ast::BlockStmt *>(s);
                for (auto &st : b->body) pre_mono_collect_in_stmt(tc, st.get());
                return;
            }
            case ast::NodeKind::VarDeclStmt: {
                auto *v = static_cast<const ast::VarDeclStmt *>(s);
                pre_mono_collect_in_type(tc, v->type.get(), v->loc);
                if (v->init) pre_mono_collect_in_expr(tc, v->init.get());
                return;
            }
            case ast::NodeKind::ExprStmt: {
                auto *e = static_cast<const ast::ExprStmt *>(s);
                pre_mono_collect_in_expr(tc, e->expr.get());
                return;
            }
            case ast::NodeKind::IfStmt: {
                auto *i = static_cast<const ast::IfStmt *>(s);
                pre_mono_collect_in_expr(tc, i->cond.get());
                pre_mono_collect_in_stmt(tc, i->then_branch.get());
                pre_mono_collect_in_stmt(tc, i->else_branch.get());
                return;
            }
            case ast::NodeKind::WhileStmt: {
                auto *w = static_cast<const ast::WhileStmt *>(s);
                pre_mono_collect_in_expr(tc, w->cond.get());
                pre_mono_collect_in_stmt(tc, w->body.get());
                return;
            }
            case ast::NodeKind::ReturnStmt: {
                auto *r = static_cast<const ast::ReturnStmt *>(s);
                if (r->value) pre_mono_collect_in_expr(tc, r->value.get());
                return;
            }
            case ast::NodeKind::ForStmt: {
                auto *f = static_cast<const ast::ForStmt *>(s);
                pre_mono_collect_in_stmt(tc, f->init.get());
                pre_mono_collect_in_expr(tc, f->cond.get());
                pre_mono_collect_in_expr(tc, f->step.get());
                pre_mono_collect_in_stmt(tc, f->body.get());
                return;
            }
            case ast::NodeKind::ThrowStmt: {
                auto *t = static_cast<const ast::ThrowStmt *>(s);
                pre_mono_collect_in_expr(tc, t->value.get());
                return;
            }
            case ast::NodeKind::TryStmt: {
                auto *t = static_cast<const ast::TryStmt *>(s);
                pre_mono_collect_in_stmt(tc, t->body.get());
                for (auto &cc : t->catches) pre_mono_collect_in_stmt(tc, cc.body.get());
                pre_mono_collect_in_stmt(tc, t->finally_body.get());
                return;
            }
            case ast::NodeKind::SynchronizedStmt: {
                auto *ss = static_cast<const ast::SynchronizedStmt *>(s);
                pre_mono_collect_in_expr(tc, ss->target.get());
                pre_mono_collect_in_stmt(tc, ss->body.get());
                return;
            }
            default: return;
        }
    }

    bool TypeChecker::run() {
        initial_errors_ = diags_.error_count();
        push_scope(); // global

        // Phase M.2.e: drenar la cola de funciones importadas via .vexi.
        // Cada entrada se declara en el scope global como Symbol::Function
        // con su sig_index ya asignado, igual que los builtins.
        for (const auto &pf : pending_imported_fn_names_) {
            Symbol s;
            s.kind      = SymbolKind::Function;
            s.sig_index = pf.second;
            (void)declare(pf.first, s);
        }
        pending_imported_fn_names_.clear();

        // Phase M.L7: drenar la cola de globals importadas.  Cada entry
        // se declara en el scope global como Symbol::Variable con su
        // tipo resuelto + flag is_const propagado del .vexi.  Si la
        // global es const + trae init_value, ademas se guarda en la
        // tabla @c imported_global_consts_ que el lowering consulta
        // para inline-ar el literal en cada uso.
        for (auto &pg : pending_imported_globals_) {
            Symbol s;
            s.kind     = SymbolKind::Variable;
            s.type     = pg.type;
            s.is_const = pg.is_const;
            (void)declare(pg.name, s);
            if (pg.is_const && pg.has_init_value) {
                ImportedGlobalConst ic;
                ic.type  = pg.type;
                ic.value = pg.init_value;
                imported_global_consts_.emplace(pg.name, std::move(ic));
            }
            // v4: comptime const string cross-module.  Guardamos los
            // bytes para que @c lower_ident pueda materializar el
            // StringObject via STRMAKE al primer uso.
            if (pg.is_str) {
                ImportedGlobalConst ic;
                ic.type      = pg.type;
                ic.is_str    = true;
                ic.str_value = std::move(pg.str_value);
                imported_global_consts_.emplace(pg.name, std::move(ic));
            }
        }
        pending_imported_globals_.clear();

        // Phase M.7: drenar la cola de namespaces.  Cada `import "x";`
        // o `import "x" as alias;` registro un namespace; aqui los
        // declaramos como Symbol::Namespace en el scope global para
        // que `lib_a.simbolo` se resuelva via check_field_access.
        for (const auto &pn : pending_imported_ns_names_) {
            Symbol s;
            s.kind     = SymbolKind::Namespace;
            s.ns_index = pn.second;
            (void)declare(pn.first, s);
        }
        pending_imported_ns_names_.clear();

        // Phase M6.a L.3: pre-pase de visibilidad para fns/globals/typedefs.
        // Las layouts struct/class/enum se setean en su procesado main
        // (que sobreescribe la entry pre-registrada con un layout fresco).
        for (const auto &decl : mod_.decls) {
            if (!decl) continue;
            switch (decl->kind) {
                case ast::NodeKind::FunctionDecl: {
                    auto *fd = static_cast<const ast::FunctionDecl *>(decl.get());
                    function_is_public_[fd->name] = fd->is_public;
                    break;
                }
                case ast::NodeKind::GlobalVarDecl: {
                    auto *gd = static_cast<const ast::GlobalVarDecl *>(decl.get());
                    global_is_public_[gd->name] = gd->is_public;
                    break;
                }
                case ast::NodeKind::TypeAliasDecl: {
                    auto *td = static_cast<const ast::TypeAliasDecl *>(decl.get());
                    typedef_is_public_[td->name] = td->is_public;
                    break;
                }
                default: break;
            }
        }

        /* si la flag de prebuilt esta seteada, intentamos
         * cargar el `.velb` cacheado en el @c comptime_runtime_ ANTES de
         * type-checar.  Esto permite que la rama @Macro VM-only (mas
         * abajo) tenga bytecode disponible al encontrar el primer call
         * site.  Cero impacto si la flag no esta o el archivo no existe
         * (la rama VM cae a AST eval). */
        if (const char *pre = std::getenv("VESTA_MC_PREBUILT")) {
            if (pre[0]) {
                std::ifstream f(pre, std::ios::binary);
                if (f) {
                    std::vector<uint8_t> bytes(
                        (std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
                    if (!bytes.empty()) {
                        const bool ok =
                            comptime_runtime_.load_macros_from_bytes(std::move(bytes));
                        if (ok) {
                            const char *verbose = std::getenv("VESTA_MC_VERBOSE");
                            if (verbose && verbose[0] == '1') {
                                std::cerr << "[mc-prebuilt] cargado: " << pre
                                          << " (" << comptime_runtime_.registered_macro_count()
                                          << " macros registrados)\n";
                            }
                        }
                    }
                }
            }
        }

        // -------- Sprint lombok (2026-06-03): expansion de anotaciones Lombok.
        // Antes de procesar templates / collect / check, recorremos cada
        // ClassDecl y generamos los ClassMethodDecls sinteticos que las
        // anotaciones Lombok piden.  Esto es un AST rewrite puro: tras el
        // pre-pase el AST se ve como si el usuario hubiera escrito los
        // metodos a mano, asi que el resto del pipeline funciona sin
        // cambios.  Implementa: @Getter, @Setter, @ToString,
        // @EqualsAndHashCode, @NoArgsConstructor, @AllArgsConstructor,
        // @RequiredArgsConstructor, @With (en field), @Data, @Value,
        // @Builder, @Synchronized, @Log.  @NonNull se traduce a la
        // sintaxis `nonnull T` del lenguaje (validacion compile-time).
        expand_lombok_annotations();

        // -------- registrar templates + monomorphizar.
        // Primero localizamos todas las clases con type_params y las
        // marcamos como templates (no se procesaran como concretas).
        for (size_t i = 0; i < mod_.decls.size(); ++i) {
            auto *d = mod_.decls[i].get();
            if (d && d->kind == ast::NodeKind::ClassDecl) {
                auto *cd = static_cast<ast::ClassDecl *>(d);
                if (!cd->type_params.empty()) {
                    generic_templates_[cd->name] = i;
                }
            } else if (d && d->kind == ast::NodeKind::EnumDecl) {
                // L2.3: enums genericos como templates.
                auto *en = static_cast<ast::EnumDecl *>(d);
                if (!en->type_params.empty()) {
                    generic_enum_templates_[en->name] = i;
                }
            }
        }
        // Snapshot del numero de decls antes de monomorphizar (las
        // monomorphizaciones nuevas se anyaden al final).  Recorremos
        // SOLO los decls originales para evitar revisitar lo nuevo.
        const size_t orig_decls = mod_.decls.size();
        for (size_t i = 0; i < orig_decls; ++i) {
            auto *d = mod_.decls[i].get();
            if (!d) continue;
            if (d->kind == ast::NodeKind::ClassDecl) {
                auto *cd = static_cast<ast::ClassDecl *>(d);
                if (!cd->type_params.empty()) continue;  // template
                for (const auto &f : cd->fields) {
                    pre_mono_collect_in_type(*this, f.type.get(), f.loc);
                }
                for (const auto &m : cd->methods) {
                    if (m->return_type)
                        pre_mono_collect_in_type(*this, m->return_type.get(), m->loc);
                    for (const auto &p : m->params)
                        pre_mono_collect_in_type(*this, p->type.get(), p->loc);
                    if (m->body) pre_mono_collect_in_stmt(*this, m->body.get());
                }
            } else if (d->kind == ast::NodeKind::FunctionDecl) {
                auto *fn = static_cast<ast::FunctionDecl *>(d);
                if (fn->return_type)
                    pre_mono_collect_in_type(*this, fn->return_type.get(), fn->loc);
                for (const auto &p : fn->params)
                    pre_mono_collect_in_type(*this, p->type.get(), p->loc);
                if (fn->body) pre_mono_collect_in_stmt(*this, fn->body.get());
            } else if (d->kind == ast::NodeKind::GlobalVarDecl) {
                auto *gv = static_cast<ast::GlobalVarDecl *>(d);
                if (gv->type) pre_mono_collect_in_type(*this, gv->type.get(), gv->loc);
                if (gv->init) pre_mono_collect_in_expr(*this, gv->init.get());
            }
        }
        // Las monomorphizaciones recien anadidas pueden a su vez
        // referenciar otros generics: re-pasamos hasta punto fijo (cota
        // razonable para evitar bucles maliciosos).
        for (int round = 0; round < 8; ++round) {
            const size_t before = mod_.decls.size();
            for (size_t i = orig_decls; i < before; ++i) {
                auto *d = mod_.decls[i].get();
                if (!d || d->kind != ast::NodeKind::ClassDecl) continue;
                auto *cd = static_cast<ast::ClassDecl *>(d);
                for (const auto &f : cd->fields) {
                    pre_mono_collect_in_type(*this, f.type.get(), f.loc);
                }
                for (const auto &m : cd->methods) {
                    if (m->return_type)
                        pre_mono_collect_in_type(*this, m->return_type.get(), m->loc);
                    for (const auto &p : m->params)
                        pre_mono_collect_in_type(*this, p->type.get(), p->loc);
                    if (m->body) pre_mono_collect_in_stmt(*this, m->body.get());
                }
            }
            if (mod_.decls.size() == before) break;
        }

        collect_globals();

        /* LANG.fix-2 pre-pase: inicializar los `comptime const|var`
         * globals con sus inits.  Sin esto, los top-level `comptime { }`
         * blocks no podrian leer ni mutar los globales (no estan en
         * comptime_const_values_ todavia).  El check_functions de mas
         * abajo skipea los globales ya inicializados aqui. */
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
            auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
            if (!gv->is_comptime || !gv->init) continue;
            const ComptimeEvalResult r =
                comptime_eval_expr(*this, gv->init.get());
            if (!r.ok) continue;  /* check_functions emitira diagnostico */
            ComptimeConst c;
            if (gv->type)         c.type = type_from_node(gv->type.get());
            else if (r.is_str)    c.type = Type{PrimitiveKind::STRING};
            else if (r.is_type)   c.type = Type{PrimitiveKind::TYPE_META};
            else                  c.type = Type{PrimitiveKind::I64};
            c.is_str    = r.is_str;
            c.is_array  = r.is_array;
            c.is_struct = r.is_struct;
            c.is_type   = r.is_type;
            c.is_mutable = !gv->is_const;
            if      (r.is_str)    c.str_value     = r.str;
            else if (r.is_array)  c.array_vals    = r.array_vals;
            else if (r.is_struct) c.struct_fields = r.struct_fields;
            else if (r.is_type)   c.type_val      = r.type_val;
            else                  c.value         = r.value;
            c.attr_align   = gv->attr_align;
            c.attr_hot     = gv->attr_hot;
            c.attr_cold    = gv->attr_cold;
            c.attr_section = gv->attr_section;
            comptime_const_values_[gv->name] = std::move(c);
        }

        /* LANG.fix-2: ejecutar bloques `comptime { ... }` a nivel modulo.
         * Tras el pre-pase los globales estan en comptime_const_values_;
         * ahora podemos evaluar/mutar via comptime_eval_stmt.  Util
         * para inicializar tablas via loops, validar invariantes con
         * static_assert sobre valores derivados, etc. */
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ComptimeBlockStmt) continue;
            check_stmt(static_cast<ast::Stmt *>(decl.get()),
                       Type{PrimitiveKind::VOID});
        }

        check_functions();
        pop_scope();

        /* log resumen de hits/misses del path VM-only si el
         * usuario activo verbose.  Util para verificar que el opt-in
         * @c VESTA_MC_VMONLY=1 + @c VESTA_MC_PREBUILT=... efectivamente
         * desvio las invocaciones @Macro al VM. */
        if (macro_vmonly_hits_ > 0 || macro_vmonly_misses_ > 0) {
            if (const char *v = std::getenv("VESTA_MC_VERBOSE")) {
                if (v[0] == '1') {
                    std::cerr << "[mc-vmonly] hits=" << macro_vmonly_hits_
                              << " misses=" << macro_vmonly_misses_
                              << "  memo_hits=" << comptime_runtime_.memo_hit_count()
                              << " memo_misses=" << comptime_runtime_.memo_miss_count()
                              << "\n";
                }
            }
        }
        return diags_.error_count() == initial_errors_;
    }

    void TypeChecker::push_scope() {
        scopes_.emplace_back();
    }
    void TypeChecker::pop_scope() {
        scopes_.pop_back();
    }

    bool TypeChecker::declare(const std::string &name, Symbol sym) {
        auto &top = scopes_.back();
        // emplace devuelve {iterator, bool} donde bool=true si se inserto.
        // Si ya existia con ese nombre en el mismo scope, reportamos colision.
        return top.emplace(name, std::move(sym)).second;
    }

    const Symbol *TypeChecker::lookup(const std::string &name) const {
        // Recorrido inverso para shadowing: scope mas interno tiene prioridad.
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    const FunctionSig *
    TypeChecker::function_sig_by_name(const std::string &name) const {
        // fix - usar el mapa @c sig_by_name_ que sobrevive al
        // @c pop_scope() final de @c run().  El @c lookup tradicional
        // falla aqui porque el scope global esta cerrado al llegar el
        // lowering.  function_sigs_ solo crece, nunca se reduce, asi que
        // los indices son estables.
        auto it = sig_by_name_.find(name);
        if (it == sig_by_name_.end()) return nullptr;
        if (it->second >= function_sigs_.size()) return nullptr;
        return &function_sigs_[it->second];
    }

    std::string
    TypeChecker::lookup_extern_qualified(const std::string &name) const {
        const FunctionSig *sig = function_sig_by_name(name);
        if (!sig) return std::string();
        if (sig->extern_lib.empty()) return std::string();
        // Formato: "@extern:<lib>:<fn>" - el cleanup distingue del nombre Vesta.
        return std::string("@extern:") + sig->extern_lib + ":" + name;
    }

    const Symbol *TypeChecker::lookup_with_depth(const std::string &name,
                                                 size_t *depth_out) const {
        // Mismo algoritmo que @c lookup pero devolviendo tambien el indice
        // del scope en la pila para que el analisis de capturas pueda
        // discriminar variables locales (depth >= lambda outer_depth) de
        // variables del entorno exterior (depth < outer_depth).
        for (size_t i = scopes_.size(); i-- > 0; ) {
            auto found = scopes_[i].find(name);
            if (found != scopes_[i].end()) {
                if (depth_out) *depth_out = i;
                // Phase M.L26: registrar el name como referenciado.  Solo
                // los lookups exitosos cuentan (los misses son errores y
                // no significan "uso valido").  El @c compile_vex_project
                // consulta este set tras run() para detectar imports sin
                // usar y emitir warnings.
                referenced_names_.insert(name);
                return &found->second;
            }
        }
        return nullptr;
    }

    Type TypeChecker::type_from_node(const ast::TypeNode *tn) const {
        if (!tn) return Type{};
        if (tn->kind == ast::NodeKind::PrimitiveTypeNode) {
            const auto *pt = static_cast<const ast::PrimitiveTypeNode *>(tn);
            Type t{pt->prim};
            // si el tipo primitivo es una coleccion y se
            // declara con type args (ej. ArrayList<string>), guardamos
            // el tipo de elemento en pointee (key en pointee, value en
            // pointee2 para los map-like).  Esto permite que el lowering
            // dispatche a las variantes *_gc cuando el elemento es GC
            // (string, class) en lugar del *_no_gc de cero overhead.
            if (!pt->type_args.empty() && is_col_kind(pt->prim)) {
                const bool is_map = (pt->prim == PrimitiveKind::HASHMAP
                                  || pt->prim == PrimitiveKind::TREEMAP);
                if (is_map) {
                    if (pt->type_args.size() >= 1) {
                        t.pointee = std::make_shared<Type>(
                            type_from_node(pt->type_args[0].get()));
                    }
                    if (pt->type_args.size() >= 2) {
                        t.pointee2 = std::make_shared<Type>(
                            type_from_node(pt->type_args[1].get()));
                    }
                } else {
                    // ARRAYLIST/HASHSET/QUEUE/DEQUE/TREESET/STACK -> 1 arg.
                    t.pointee = std::make_shared<Type>(
                        type_from_node(pt->type_args[0].get()));
                }
            }
            // Smart pointers: unique<T> / shared<T> almacenan el tipo del
            // recurso apuntado en @c pointee (analogo a Optional<T>).
            // Sin esto, `unique<i32>` quedaria como kind=UNIQUE_PTR sin
            // pointee, y la asignacion `unique<i32> p = unique_box(42)`
            // fallaria al unificar.
            if (!pt->type_args.empty()
             && (pt->prim == PrimitiveKind::UNIQUE_PTR
              || pt->prim == PrimitiveKind::SHARED_PTR
              || pt->prim == PrimitiveKind::BORROW
              || pt->prim == PrimitiveKind::BORROW_MUT)) {
                t.pointee = std::make_shared<Type>(
                    type_from_node(pt->type_args[0].get()));
            }
            return t;
        }
        if (tn->kind == ast::NodeKind::NamedTypeNode) {
            const auto *nt = static_cast<const ast::NamedTypeNode *>(tn);
            // Phase M.7.c: namespace qualified type (`ui.Button`).
            // El parser concatena los segmentos con `.`; lo separamos
            // y resolvemos buscando primero el namespace local, luego
            // el simbolo dentro.  Si encaja, traducimos a Type del
            // tipo apuntado (con el mangled label correspondiente).
            {
                size_t dot = nt->name.find('.');
                if (dot != std::string::npos) {
                    const std::string ns_name = nt->name.substr(0, dot);
                    const std::string sym_name = nt->name.substr(dot + 1);
                    // LANG.fix-3: resolver via mapa persistente
                    // ns_idx_by_local_name_ que sobrevive al pop_scope
                    // del final de tc.run().  Si esa busqueda falla,
                    // fallback al lookup tradicional (durante check
                    // phase los scopes aun estan vivos).
                    uint32_t ns_idx_resolved = UINT32_MAX;
                    auto it_ns = ns_idx_by_local_name_.find(ns_name);
                    if (it_ns != ns_idx_by_local_name_.end()) {
                        ns_idx_resolved = it_ns->second;
                    } else {
                        const Symbol *ns_sym = lookup(ns_name);
                        if (ns_sym && ns_sym->kind == SymbolKind::Namespace) {
                            ns_idx_resolved = ns_sym->ns_index;
                        }
                    }
                    if (ns_idx_resolved < imported_namespaces_.size()) {
                        const auto &ns = imported_namespaces_[ns_idx_resolved];
                        auto its = ns.by_name.find(sym_name);
                        if (its != ns.by_name.end()) {
                            const auto &sym = ns.symbols[its->second];
                            // El mangled_label es el nombre interno
                            // (e.g. `ui__Button`).  Buscamos el layout
                            // en struct/class/enum layouts.
                            auto it_cls = class_layouts_.find(sym.mangled_label);
                            if (it_cls != class_layouts_.end()) {
                                return Type{PrimitiveKind::CLASS, sym.mangled_label};
                            }
                            auto it_st = struct_layouts_.find(sym.mangled_label);
                            if (it_st != struct_layouts_.end()) {
                                return Type{PrimitiveKind::STRUCT, sym.mangled_label};
                            }
                            auto it_en = enum_layouts_.find(sym.mangled_label);
                            if (it_en != enum_layouts_.end()) {
                                return Type{PrimitiveKind::STRUCT, sym.mangled_label};
                            }
                            auto it_ta = type_aliases_.find(sym.mangled_label);
                            if (it_ta != type_aliases_.end()) {
                                return it_ta->second;
                            }
                        }
                    }
                }
            }
            /* si el nombre esta bindeado como comptime type-param
             * (estamos dentro de un call a comptime fn generica), lo
             * sustituimos por el tipo concreto.  Esto permite que
             * `sizeof<T>()` dentro del body resuelva al tipo proveido en
             * el call site. */
            {
                Type bound;
                if (lookup_comptime_type(nt->name, bound)) {
                    return bound;
                }
            }
            /* Type-as-first-class-value.  Si el nombre matchea un
             * `comptime const Type X = comptime_type<...>()` global, el
             * `type_val` cacheado contiene el Type real -> sustituir.
             * Permite usar `X` en cualquier posicion de tipo. */
            {
                auto it_ct = comptime_const_values_.find(nt->name);
                if (it_ct != comptime_const_values_.end()
                 && it_ct->second.is_type
                 && it_ct->second.type_val.kind != PrimitiveKind::TYPE_META) {
                    return it_ct->second.type_val;
                }
            }
            /* identifier `Type` actua como sentinela TYPE_META.  El
             * caller (check_var_decl + lower_var_decl) se encarga del
             * binding real (sin storage runtime). */
            if (nt->name == "Type" && nt->type_args.empty()) {
                return Type{PrimitiveKind::TYPE_META};
            }
            // -1) Builtins genericos del compilador: Optional<T> y
            //     Result<V, E> NO se monomorphizan; el type checker los
            //     recoge como tipos especiales con layout fijo, y el
            //     lowering emite directamente el codigo optimizado.  Esto
            //     evita que cada uso genere una clase concreta nueva en
            //     el bytecode (ahorro masivo en proyectos grandes).
            if (nt->name == "Optional" && nt->type_args.size() == 1) {
                return Type::make_optional(type_from_node(nt->type_args[0].get()));
            }
            if (nt->name == "Result" && nt->type_args.size() == 2) {
                return Type::make_result(
                    type_from_node(nt->type_args[0].get()),
                    type_from_node(nt->type_args[1].get()));
            }
            // Mejora II: Future<T> es builtin igual que Optional/Result.
            // El frontend lo modela con kind=FUTURE + pointee=T.  El
            // bytecode no cambia: el handle sigue siendo i64 opaco.
            if (nt->name == "Future" && nt->type_args.size() == 1) {
                return Type::make_future(type_from_node(nt->type_args[0].get()));
            }
            // 0) Generics: si tiene type_args, mapeamos al mangled name
            //    (la monomorphizacion ya se hizo en el pre-pase).
            std::string lookup = nt->name;
            if (!nt->type_args.empty()) {
                std::vector<Type> args;
                args.reserve(nt->type_args.size());
                for (auto &ta : nt->type_args) {
                    args.push_back(type_from_node(ta.get()));
                }
                lookup = nt->name + "_" + mangle_args(args);
            }
            // 1) Alias resolution.
            auto it_a = type_aliases_.find(lookup);
            if (it_a != type_aliases_.end()) {
                referenced_names_.insert(lookup);   // L.26: type alias usado
                return it_a->second;
            }
            // 2) Struct registrado: devolvemos Type{STRUCT, name}.
            auto it_s = struct_layouts_.find(lookup);
            if (it_s != struct_layouts_.end()) {
                referenced_names_.insert(lookup);   // L.26: struct usado
                return Type{PrimitiveKind::STRUCT, lookup};
            }
            // enum registrado.  Reusamos PrimitiveKind::STRUCT
            // con struct_name = nombre del enum: el lowering distingue
            // mirando enum_layouts_ vs struct_layouts_.  Esto evita
            // anyadir un nuevo PrimitiveKind para no inflar el switch
            // ubicuo del IR / lowering, manteniendo la semantica
            // value-type igual que un struct (alocado en stack del scope
            // que lo crea, copia por valor).
            auto it_e = enum_layouts_.find(lookup);
            if (it_e != enum_layouts_.end()) {
                referenced_names_.insert(lookup);   // L.26: enum usado
                return Type{PrimitiveKind::STRUCT, lookup};
            }
            // 3) Clase registrada: devolvemos Type{CLASS, name}.  CLASS
            //    es reference type: variables del tipo son punteros al
            //    ObjectHeader, instances se crean con NEWOBJ.
            auto it_c = class_layouts_.find(lookup);
            if (it_c != class_layouts_.end()) {
                referenced_names_.insert(lookup);   // L.26: class usada
                return Type{PrimitiveKind::CLASS, lookup};
            }
            // 4) Si el original (no mangled) ESTA en class_layouts, lo
            //    devolvemos: probablemente es uso de una clase concreta
            //    pasada como type arg.  Solo recurrimos a este fallback
            //    cuando habia type_args (sin args es uso normal).
            if (!nt->type_args.empty()) {
                auto it_cb = class_layouts_.find(nt->name);
                if (it_cb != class_layouts_.end()) {
                    return Type{PrimitiveKind::CLASS, nt->name};
                }
            }
            // 5) Tipo desconocido.
            return Type{};
        }
        if (tn->kind == ast::NodeKind::PointerTypeNode) {
            const auto *pn = static_cast<const ast::PointerTypeNode *>(tn);
            // Resolver recursivamente el tipo apuntado y envolverlo en PTR.
            Type pointee = type_from_node(pn->pointee.get());
            // No exigimos que pointee este resuelto aqui (un puntero a un
            // tipo desconocido produce VOID-pointee, que el callsite
            // detectara cuando intente desreferenciar).
            return Type::make_ptr(std::move(pointee), pn->is_virtual);
        }
        if (tn->kind == ast::NodeKind::ArrayTypeNode) {
            const auto *an = static_cast<const ast::ArrayTypeNode *>(tn);
            Type elem = type_from_node(an->element_type.get());
            uint32_t size = 0;
            if (an->size_expr) {
                // Solo aceptamos literal entero positivo en.  Ampliar
                // a expresiones constantes (con const-folding) requiere un
                // evaluador en el frontend; pendiente para hitos posteriores.
                if (an->size_expr->kind == ast::NodeKind::IntLitExpr) {
                    auto *lit = static_cast<const ast::IntLitExpr *>(an->size_expr.get());
                    size = static_cast<uint32_t>(lit->value);
                } else {
                    // El caller emite el diagnostico apropiado mas tarde
                    // (typicamente en check_var_decl con loc preciso).
                    size = 0;
                }
            }
            return Type::make_array(std::move(elem), size);
        }
        // `fn(P1, P2) -> R` -> Type{FUNCTION, [P1,P2], R}.
        // El parser garantiza que return_type nunca es null (usa VOID por
        // defecto cuando el usuario omite la flecha @c ->), asi que no
        // necesitamos chequear nullptr aqui.
        if (tn->kind == ast::NodeKind::FunctionTypeNode) {
            const auto *fn = static_cast<const ast::FunctionTypeNode *>(tn);
            std::vector<Type> params;
            params.reserve(fn->param_types.size());
            for (auto &p : fn->param_types) {
                params.push_back(type_from_node(p.get()));
            }
            Type ret = type_from_node(fn->return_type.get());
            return Type::make_function(std::move(params), std::move(ret));
        }
        return Type{};
    }

    // ---------------------------------------------------------------------
    // Pase 1: declaraciones globales.
    // ---------------------------------------------------------------------

    void TypeChecker::collect_globals() {
        // ----- Builtins predefinidos -----
        //
        // Declarados con firmas que el type checker valida por unificacion
        // exacta (PTR == PTR, I64 == I64, ...).  El lowering intercepta
        // las llamadas a estos nombres en try_lower_builtin_call() y emite
        // la convencion FFI correspondiente a vesta_io.
        //
        // Restriccion: los argumentos de tipo PTR deben ser literales
        // de string directos (StringLitExpr), porque para los FFI
        // necesitamos la longitud en compile-time.  El type checker NO
        // exige esto (le basta con el tipo PTR); el lowering lo verifica
        // por su lado y reporta error claro si el arg no es literal.
        auto reg_builtin = [&](const std::string &name,
                               Type ret,
                               std::initializer_list<PrimitiveKind> params) {
            FunctionSig sig;
            sig.return_type = ret;
            sig.param_types.reserve(params.size());
            for (auto p : params) sig.param_types.push_back(Type{p});
            Symbol s;
            s.kind      = SymbolKind::Function;
            s.sig_index = (uint32_t)function_sigs_.size();
            sig_by_name_[name] = s.sig_index;
            function_sigs_.push_back(std::move(sig));
            (void)declare(name, s);
        };
        // Salida de texto (aceptan ANY tipo via dispatch en lowering).
        // El check_call hace bypass especial para estos nombres y permite
        // string interpolado (StringLitExpr con interp_exprs) o escalares.
        reg_builtin("print",     Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("println",   Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("echo",      Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        // flush() sin argumentos (vacia el buffer de vesta_io).
        reg_builtin("flush",     Type{PrimitiveKind::VOID}, {});
        // Salida de valores numericos (sin acceso a memoria VM).
        reg_builtin("print_int",   Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});
        reg_builtin("print_uint",  Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_hex",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_float", Type{PrimitiveKind::VOID}, {PrimitiveKind::F64});
        reg_builtin("print_bool",  Type{PrimitiveKind::VOID}, {PrimitiveKind::BOOL});
        reg_builtin("print_char",  Type{PrimitiveKind::VOID}, {PrimitiveKind::U32});
        reg_builtin("print_color", Type{PrimitiveKind::VOID}, {PrimitiveKind::U32});
        // print_cstr(host_ptr) imprime una cstring desde memoria host
        // (FatalError.message o .stack_trace).  Acepta cualquier PTR.
        reg_builtin("print_cstr",  Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        // Formatos numericos alternativos: binario y octal con prefijo
        // "0b" / "0o".  Compactos (sin ceros lider).
        reg_builtin("print_bin",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_oct",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        // print_ptr(addr) imprime "0x<hex>" compacto sin ceros lider.
        // Acepta cualquier PTR (host o virtual) o un i64 con la direccion.
        // print_gchandle(handle) imprime "<gc:N>" donde N es el handle
        // como entero decimal.  Para uso con CLASS objects el lowering
        // emite la instruccion @c gchandle antes de llamar.
        reg_builtin("print_ptr",      Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("print_gchandle", Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});
        // Padding/alineacion: emite @p width copias del codepoint
        // @p fill_cp.  El usuario calcula la diferencia entre el ancho
        // deseado y el ancho actual del texto y llama a este builtin.
        // Para alineacion manual de columnas en TUIs.
        reg_builtin("print_pad",   Type{PrimitiveKind::VOID},
                    {PrimitiveKind::U32, PrimitiveKind::U64});
        // I/O de fichero.  fopen recibe path y modo como literales de
        // string; devuelve un FILE* (uint64_t).  fwrite recibe el FILE*
        // y un buffer literal; devuelve el numero de bytes escritos.
        // fclose recibe el FILE* y devuelve un codigo i32.
        reg_builtin("fopen",  Type{PrimitiveKind::I64},
                    {PrimitiveKind::PTR, PrimitiveKind::PTR});
        reg_builtin("fwrite", Type{PrimitiveKind::I64},
                    {PrimitiveKind::I64, PrimitiveKind::PTR});
        reg_builtin("fclose", Type{PrimitiveKind::I32},
                    {PrimitiveKind::I64});

        // Allocator manual: malloc devuelve void* (puntero host obtenido del
        // RawAllocator del proceso); free libera un puntero anteriormente
        // devuelto por malloc.  El usuario calcula los bytes manualmente
        // (sizeof aun no implementado): malloc(4 * 10) reserva 10 i32s.
        // El lowering marca el resultado de malloc como is_host_ptr=true
        // para que LOAD/STORE emitan `movh` (acceso a memoria host).
        // free admite cualquier T* (sin chequeo dinamico de tipo).
        reg_builtin("malloc", Type::make_ptr(Type{PrimitiveKind::VOID}),
                    {PrimitiveKind::I64});
        reg_builtin("free",   Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});

        // Alias predefinido `cstring` = `char*`.  Permite
        // declarar `cstring p` para FFI con char* sin tener que escribir
        // `char* p`.  Se registra como type alias (typedef) global; el
        // type checker resuelve `cstring` a `Type{PTR, pointee=CHAR}`.
        {
            Type t_char = Type{PrimitiveKind::CHAR};
            Type t_cstring = Type::make_ptr(t_char);
            type_aliases_["cstring"] = t_cstring;
        }

        // Alias predefinidos para reflexion: Class / Method / Field / Object.
        // Se resuelven a i64 (handle opaco del ClassRegistry / MethodInfo* /
        // FieldInfo* / host_ptr del objeto).  Cuando se declara una variable
        // con uno de estos tipos (e.g. `Class cls = forName("X")`), el type
        // checker registra el alias en el Symbol; las llamadas
        // `cls.getMethod("foo")` se desazucaran a `getMethod(cls, "foo")`.
        // Esto provee sintaxis OO ergonomica sin cambios en el runtime.
        {
            const Type t_i64 = Type{PrimitiveKind::I64};
            type_aliases_["Class"]  = t_i64;
            type_aliases_["Method"] = t_i64;
            type_aliases_["Field"]  = t_i64;
            type_aliases_["Object"] = t_i64;
        }

        // Builtins de string operando sobre StringObject (tipo
        // STRING).  Todos se bajan a un solo opcode bytecode (~5 ns).
        reg_builtin("str_length", Type{PrimitiveKind::I64}, {PrimitiveKind::STRING});
        reg_builtin("str_bytes",  Type{PrimitiveKind::I64}, {PrimitiveKind::STRING});
        reg_builtin("str_cstr",   Type{PrimitiveKind::PTR}, {PrimitiveKind::STRING});
        reg_builtin("str_wstr",   Type{PrimitiveKind::PTR}, {PrimitiveKind::STRING});
        reg_builtin("str_hash",   Type{PrimitiveKind::U64}, {PrimitiveKind::STRING});
        reg_builtin("str_intern", Type{PrimitiveKind::STRING}, {PrimitiveKind::STRING});
        reg_builtin("str_concat", Type{PrimitiveKind::STRING}, {PrimitiveKind::STRING, PrimitiveKind::STRING});
        reg_builtin("str_equals", Type{PrimitiveKind::BOOL},   {PrimitiveKind::STRING, PrimitiveKind::STRING});
        reg_builtin("str_make",   Type{PrimitiveKind::STRING}, {PrimitiveKind::PTR, PrimitiveKind::I64});
        // Encoding explicito.  str_convert(s, enc) usa
        // STRCONV bytecode.  Acepta cualquier int de las constantes
        // ENC_* (ASCII=0, ANSI=1, UTF8=2, UTF16=3, UTF32=4) declaradas
        // como Symbol::Constant abajo.  Util para preparar wstr (UTF16)
        // antes de pasar a Win32 *W APIs.
        reg_builtin("str_convert", Type{PrimitiveKind::STRING},
                    {PrimitiveKind::STRING, PrimitiveKind::I32});

        // Constantes de encoding (idem ANSI codes: registradas como
        // Symbol::Constant con tipo i32).  El lowering las inlinea como
        // const literal sin lookup runtime.
        auto reg_const_int = [&](const std::string &name, int32_t value) {
            Symbol s;
            s.kind = SymbolKind::Constant;
            s.type = Type{PrimitiveKind::I32};
            // Reusamos campo type como contenedor + valor en sig_index
            // (truco: sig_index es uint32_t y vale como inmediato).
            s.sig_index = static_cast<uint32_t>(value);
            (void)declare(name, s);
        };
        reg_const_int("ENC_ASCII", 0);
        reg_const_int("ENC_ANSI",  1);
        reg_const_int("ENC_UTF8",  2);
        reg_const_int("ENC_UTF16", 3);
        reg_const_int("ENC_UTF32", 4);

        // panic("msg") lanza FatalError(USER_ABORT, msg).  Si hay
        // try/catch FatalError lo captura; si no, mata el proceso (no la VM).
        reg_builtin("panic",  Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});

        // dispose(xs) libera explicitamente una coleccion antes
        // del exit del scope.  Tras la llamada, el local queda con handle=0
        // y el cleanup automatico llama free fn que es no-op con handle=0.
        // Aceptamos cualquier tipo coleccion (bypass relax en check_call).
        reg_builtin("dispose", Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});

        // Constructores de tipos primitivos de coleccion.
        // Cada uno acepta una capacidad inicial opcional (i64, default 16);
        // por simplicidad, registramos la signature con el arg requerido.
        // El usuario llama @c arraylist(N) (N >= 16 internamente capado) o
        // @c arraylist() (caso 0-args sin registrar firma extra: usamos el
        // bypass relajado en check_call analogo a print).  Para esta primera
        // iteracion exigimos siempre 1 arg explicito o wrapper Vex; suficiente
        // para validar la integracion.
        for (size_t i = 0; i < COL_TYPES_N; ++i) {
            const ColType &ct = COL_TYPES[i];
            // arraylist(i64) -> ArrayList     (constructor con capacidad)
            // tmap_new() / tset_new() son sin args; los tratamos igual:
            // el lowering ignora args extra cuando default_cap == 0.
            if (ct.default_cap == 0) {
                reg_builtin(ct.vex_ctor_name, Type{ct.kind}, {});
            } else {
                reg_builtin(ct.vex_ctor_name, Type{ct.kind}, {PrimitiveKind::I64});
            }
        }

        // FFI runtime dinamico estilo VSH.  Permite cargar DLLs y
        // resolver simbolos en runtime (vs el FFI declarativo extern, que
        // resuelve en compile-time).  Cero overhead de la ruta normal:
        // solo paga cuando el usuario decide usarlo.
        //   ffi_open(string path) -> i64 handle
        //   ffi_sym (i64 handle, string name) -> i64 fn_addr
        //   ffi_call(i64 fn_addr, ...args) -> i64 result   (variadic 0-12)
        // El check_call hace bypass para ffi_call (acepta argc variable).
        reg_builtin("ffi_open", Type{PrimitiveKind::I64}, {PrimitiveKind::PTR});
        reg_builtin("ffi_sym",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::PTR});
        reg_builtin("ffi_call", Type{PrimitiveKind::I64}, {PrimitiveKind::I64});

        // Math builtins delegando a stdlib/native/math/vesta_math.dll.  El
        // ABI nativo es uint64_t -> uint64_t con bits IEEE 754; el lowering
        // emite CALLN directo a vmath_<name> (no pasa por IR ops float).
        // Cubrir todo lo expuesto por vmath_*: scalar y trig.
        reg_builtin("sqrt",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("pow",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("fabs",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("floor", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("ceil",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("round", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("fmin",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("fmax",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("log",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("log2",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("log10", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("sin",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("cos",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("tan",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("abs",   Type{PrimitiveKind::I64}, {PrimitiveKind::I64});
        reg_builtin("imin",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64});
        reg_builtin("imax",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64});
        reg_builtin("clamp", Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64, PrimitiveKind::I64});
        // Math-IR-promote v2.2a: float + bit ops nuevos.
        reg_builtin("trunc",    Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("iminu",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("imaxu",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("ilog2",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("popcount", Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("clz",      Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("ctz",      Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("bswap",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("rotl",     Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("rotr",     Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});

        /* Sprint B.1: callback Vex -> C nativo.  Toma una fn Vex como
         * argumento y devuelve un host_ptr (i64) a un thunk con cc C
         * estandar (Win64 o SysV).  El check_call hace bypass especial
         * para validar que el arg es una IdentExpr a una funcion
         * declarada (no a un lambda value).  Sintaxis:
         *
         *   i32 mi_cmp(u8* a, u8* b) { ... }
         *   u64 cb = as_native_callback(mi_cmp);
         *   qsort(arr, n, sz, cb);  // C llama a mi_cmp via cc nativa */
        reg_builtin("as_native_callback", Type{PrimitiveKind::I64}, {PrimitiveKind::PTR});

        // Identificadores magicos para colores y atributos ANSI.
        //
        // Registramos como Symbols globales tipo PTR (string).  El
        // lowering los detecta en lower_ident y emite la secuencia ANSI
        // correspondiente como string literal estatico (zero overhead).
        // Lista cubre los SGR mas comunes: 8 colores foreground + 8
        // brillantes + atributos de estilo + RESET + dos helpers de
        // pantalla (CLEAR_SCREEN, CURSOR_HOME).  Todos quedan en el
        // scope global y se ven desde cualquier funcion.
        auto reg_const_string = [&](const std::string &name) {
            Symbol s;
            s.kind = SymbolKind::Constant;
            s.type = Type{PrimitiveKind::PTR};
            (void)declare(name, s);
        };
        // Foreground (regulares 30..37 y brillantes 90..97).
        reg_const_string("BLACK");        reg_const_string("RED");
        reg_const_string("GREEN");        reg_const_string("YELLOW");
        reg_const_string("BLUE");         reg_const_string("MAGENTA");
        reg_const_string("CYAN");         reg_const_string("WHITE");
        reg_const_string("BR_BLACK");     reg_const_string("BR_RED");
        reg_const_string("BR_GREEN");     reg_const_string("BR_YELLOW");
        reg_const_string("BR_BLUE");      reg_const_string("BR_MAGENTA");
        reg_const_string("BR_CYAN");      reg_const_string("BR_WHITE");
        // Background (40..47).
        reg_const_string("BG_BLACK");     reg_const_string("BG_RED");
        reg_const_string("BG_GREEN");     reg_const_string("BG_YELLOW");
        reg_const_string("BG_BLUE");      reg_const_string("BG_MAGENTA");
        reg_const_string("BG_CYAN");      reg_const_string("BG_WHITE");
        // Atributos de estilo y reset.
        reg_const_string("BOLD");         reg_const_string("DIM");
        reg_const_string("ITALIC");       reg_const_string("UNDERLINE");
        reg_const_string("BLINK");        reg_const_string("REVERSE");
        reg_const_string("RESET");
        // Helpers de pantalla (utiles para TUIs).
        reg_const_string("CLEAR_SCREEN"); reg_const_string("CURSOR_HOME");

        // registrar @c FatalError como clase pre-definida en
        // runtime.  El @c init_exception_classes del runtime ya la creo
        // en el ClassRegistry; aqui solo damos al type checker / lowering
        // la metadata para validar @c catch (FatalError e) y @c e.kind /
        // @c e.message / @c e.stack_trace.  Marcada con
        // @c is_runtime_predefined=true para que el lowering NO emita
        // defclass / __new_FatalError / etc.
        {
            ClassLayout fe;
            fe.name = "FatalError";
            fe.is_runtime_predefined = true;
            // ABI fija (matching exception_runtime.h):
            //   +24 i32 kind  (size 8 con padding)
            //   +32 u64 pc
            //   +40 ptr message
            //   +48 ptr stack_trace
            // Total 56 bytes (ObjectHeader 24 + 4*8).
            auto add_field = [&](const char *name, PrimitiveKind k,
                                 uint32_t off) {
                StructFieldInfo fi;
                fi.name      = name;
                fi.type      = Type{k};
                fi.offset    = off;
                fi.size      = 8;
                fe.fields.push_back(fi);
            };
            add_field("kind",        PrimitiveKind::I32, 24);
            add_field("pc",          PrimitiveKind::U64, 32);
            add_field("message",     PrimitiveKind::PTR, 40);
            add_field("stack_trace", PrimitiveKind::PTR, 48);
            fe.size_bytes = 56;
            class_layouts_["FatalError"] = std::move(fe);
        }

        // BugFix R4: registrar clases excepcion estandar (sincronizado con
        // exception_runtime.cpp::init_exception_classes).  Cada una tiene
        // un solo field `message` (string ptr) en offset 24 (despues del
        // ObjectHeader).  El lowering del frontend detecta
        // `new <ExceptionClass>(msg)` y emite la secuencia inline
        // (newobj + store message at +24).  El catch puede leer
        // `e.message` via getfield estandar.
        {
            const char *std_exc_names[] = {
                "RuntimeException",
                "ArithmeticException",
                "IllegalArgumentException",
                "IndexOutOfBoundsException",
                "NullPointerException",
                "IOException",
                "ClassCastException",
                "UnsupportedOperationException",
            };
            for (const char *n : std_exc_names) {
                ClassLayout cl;
                cl.name = n;
                cl.is_runtime_predefined = true;
                StructFieldInfo fi;
                fi.name   = "message";
                fi.type   = Type{PrimitiveKind::PTR};
                fi.offset = 24;
                fi.size   = 8;
                cl.fields.push_back(fi);
                cl.size_bytes = 32;
                class_layouts_[n] = std::move(cl);
            }
        }

        // Pre-pasada (robustness): registrar nombres de TODOS
        // los structs / clases / enums con layouts mininos (vacio) ANTES
        // del procesamiento real de campos y metodos.  Esto permite:
        //   - Self-references: `class Node { Node next; }`
        //   - Forward refs: `class A { B b; }  class B { A a; }`
        //   - Mutual recursion entre clases en cualquier orden.
        // Sin esto, type_from_node falla al ver el primer uso del
        // nombre y devuelve VOID, propagando errores en cascada.
        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::StructDecl) {
                auto *s = static_cast<ast::StructDecl *>(decl.get());
                if (!struct_layouts_.count(s->name)) {
                    StructLayout empty;
                    empty.name          = s->name;
                    empty.is_introspect = s->is_introspect;
                    struct_layouts_.emplace(s->name, std::move(empty));
                }
            } else if (decl->kind == ast::NodeKind::ClassDecl) {
                auto *c = static_cast<ast::ClassDecl *>(decl.get());
                // Templates con type_params no son clases concretas.
                if (!c->type_params.empty()) continue;
                if (!class_layouts_.count(c->name)) {
                    ClassLayout empty;
                    empty.name          = c->name;
                    empty.is_interface  = c->is_interface;
                    empty.is_aspect     = c->is_aspect;
                    empty.is_introspect = c->is_introspect;
                    class_layouts_.emplace(c->name, std::move(empty));
                }
            } else if (decl->kind == ast::NodeKind::EnumDecl) {
                auto *en = static_cast<ast::EnumDecl *>(decl.get());
                // L2.3: enums template (con type_params) NO se registran
                // como concretos; se monomorphizan on demand.
                if (!en->type_params.empty()) continue;
                if (!enum_layouts_.count(en->name)) {
                    EnumLayout empty;
                    empty.name          = en->name;
                    empty.is_introspect = en->is_introspect;
                    enum_layouts_.emplace(en->name, std::move(empty));
                }
            }
        }
        // Pase 0: registrar typedef/using y struct ANTES de funciones y
        // globales, para que cualquier referencia posterior a esos nombres
        // se resuelva correctamente.  Procesar typedef/using en orden
        // permite alias anidados (typedef A B; typedef B C;) si A esta
        // declarado antes que B; alias adelantados (B antes de A) no se
        // resuelven y emiten error.
        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::TypeAliasDecl) {
                auto *a = static_cast<ast::TypeAliasDecl *>(decl.get());
                Type resolved = type_from_node(a->aliased.get());
                if (resolved.kind == PrimitiveKind::COUNT
                 || (resolved.kind == PrimitiveKind::VOID
                     && a->aliased
                     && a->aliased->kind == ast::NodeKind::NamedTypeNode)) {
                    // No se pudo resolver el tipo subyacente: nombre
                    // desconocido o forward-reference.
                    diags_.error(a->loc,
                        "tipo no resuelto en alias '" + a->name + "'");
                    continue;
                }
                // Newtype: asignar nominal_id unico para que sea
                // nominalmente distinto del underlying y de otros newtypes
                // con misma representacion.  Sin esto, `typedef u64 fd new`
                // y `typedef u64 port new` serian tipos identicos (ambos
                // u64) -- exactamente lo que queremos EVITAR.
                if (a->is_newtype) {
                    resolved.nominal_id    = ++newtype_counter_;
                    resolved.nominal_name  = a->name;
                    resolved.is_opaque     = a->is_opaque;
                    resolved.align_override = a->align_override;
                    // Underlying preservado (para introspeccion + cast
                    // explicito).  Conservamos COPIA del underlying en
                    // newtype_underlying_ para que `typedef is T` y
                    // `(T)x` puedan responder sin parsear el AST.
                    Type underlying = resolved;
                    underlying.nominal_id     = 0;
                    underlying.is_opaque      = false;
                    underlying.align_override = 0;
                    underlying.nominal_name.clear();
                    newtype_underlying_.emplace(a->name, std::move(underlying));
                    // Registrar conversiones permitidas + fichero de
                    // declaracion (para module-privacy de @opaque).
                    NewtypeInfo info;
                    info.source_file = a->loc.file;
                    for (auto &ec : a->explicit_from) {
                        if (!ec.type) continue;
                        Type t = type_from_node(ec.type.get());
                        if (t.kind == PrimitiveKind::COUNT
                         || t.kind == PrimitiveKind::VOID) {
                            diags_.error(a->loc,
                                "tipo no resuelto en 'explicit from' de '"
                                + a->name + "'");
                            continue;
                        }
                        info.from_conversions.push_back({std::move(t), ec.is_public});
                    }
                    for (auto &ec : a->explicit_to) {
                        if (!ec.type) continue;
                        Type t = type_from_node(ec.type.get());
                        if (t.kind == PrimitiveKind::COUNT
                         || t.kind == PrimitiveKind::VOID) {
                            diags_.error(a->loc,
                                "tipo no resuelto en 'explicit to' de '"
                                + a->name + "'");
                            continue;
                        }
                        info.to_conversions.push_back({std::move(t), ec.is_public});
                    }
                    newtype_info_.emplace(a->name, std::move(info));
                }
                if (!type_aliases_.emplace(a->name, resolved).second) {
                    diags_.error(a->loc, "alias de tipo redefinido: '" + a->name + "'");
                }
            } else if (decl->kind == ast::NodeKind::StructDecl) {
                auto *s = static_cast<ast::StructDecl *>(decl.get());
                // Pre-pasada (mas arriba) ya creo una entrada vacia.  Si
                // size_bytes > 0 (ya completada) -> redeclaracion real.
                auto it_pre = struct_layouts_.find(s->name);
                if (it_pre != struct_layouts_.end() && !it_pre->second.fields.empty()) {
                    diags_.error(s->loc, "struct redeclarado: '" + s->name + "'");
                    continue;
                }

                // Calcular layout con alineamiento natural por campo, igual
                // que C: cada campo arranca en el siguiente offset que sea
                // multiplo de su sizeof.  El struct entero tambien queda
                // alineado al campo mas grande, redondeando size_bytes
                // al final para que arrays de structs tengan offset
                // consistente entre elementos.
                StructLayout layout;
                layout.name = s->name;
                uint32_t offset = 0;
                uint32_t max_align = 1;
                std::unordered_map<std::string, bool> seen_names;

                // Estado para packing de bit fields.
                //   bf_active=true mientras hay un storage word abierto.
                //   bf_offset: offset (bytes) del storage word actual.
                //   bf_size: tamano (bytes) del storage word.
                //   bf_used: bits ya usados dentro del word.
                bool     bf_active = false;
                uint32_t bf_offset = 0, bf_size = 0;
                uint8_t  bf_used   = 0;
                auto close_bf = [&]() {
                    if (bf_active) {
                        offset = bf_offset + bf_size;
                        bf_active = false;
                        bf_used = 0;
                    }
                };

                for (const auto &f : s->fields) {
                    if (!seen_names.emplace(f.name, true).second) {
                        diags_.error(f.loc,
                            "campo duplicado en struct '" + s->name + "': '" + f.name + "'");
                        continue;
                    }
                    Type ft = type_from_node(f.type.get());
                    if (ft.kind == PrimitiveKind::COUNT
                     || ft.kind == PrimitiveKind::VOID) {
                        diags_.error(f.loc,
                            "tipo invalido en campo '" + f.name + "' del struct '" + s->name + "'");
                        continue;
                    }
                    // Tamano y alineamiento del campo.  Para STRUCT anidado
                    // consultamos su propio layout previamente registrado;
                    // si aun no se registro (forward ref dentro del mismo
                    // pase) emitimos error de orden de declaracion.
                    uint32_t fsize = (uint32_t)primitive_size_bytes(ft.kind);
                    uint32_t falign = fsize;
                    if (ft.kind == PrimitiveKind::STRUCT) {
                        auto it = struct_layouts_.find(ft.struct_name);
                        if (it == struct_layouts_.end()) {
                            diags_.error(f.loc,
                                "struct '" + ft.struct_name +
                                "' debe declararse antes de usarse como campo");
                            continue;
                        }
                        fsize  = it->second.size_bytes;
                        falign = it->second.align_bytes;
                    }
                    if (fsize == 0) {
                        // Defensa: deberia haber sido atrapado arriba.
                        fsize = 1;
                    }
                    if (falign == 0) falign = 1;
                    // Newtype con @align(N): forzar alineacion del campo.
                    // El tamano sigue siendo el del underlying (el align
                    // solo afecta el padding antes del campo + el align
                    // del struct contenedor).
                    if (ft.align_override > 0 && ft.align_override > falign) {
                        falign = ft.align_override;
                    }

                    // Bit field handling.
                    if (f.bit_width > 0) {
                        // Solo permitido sobre tipos integer.
                        if (!is_integral(ft.kind)) {
                            diags_.error(f.loc,
                                "bit field '" + f.name + "' requiere tipo integer");
                            continue;
                        }
                        const uint8_t bw = f.bit_width;
                        const uint8_t cap = (uint8_t)(fsize * 8u);
                        if (bw > cap) {
                            diags_.error(f.loc,
                                "bit_width (" + std::to_string((int)bw)
                                + ") excede el tamano del tipo (" + std::to_string((int)cap) + ")");
                            continue;
                        }
                        // Si hay storage abierto del MISMO tamano y queda hueco,
                        // empaquetar.  Si no, abrir nuevo storage en posicion
                        // alineada.
                        if (!bf_active || bf_size != fsize
                         || (bf_used + bw) > cap) {
                            close_bf();
                            // Padding hasta multiplo de falign.
                            if (offset % falign != 0) {
                                offset += falign - (offset % falign);
                            }
                            bf_active = true;
                            bf_offset = offset;
                            bf_size   = fsize;
                            bf_used   = 0;
                            if (falign > max_align) max_align = falign;
                        }
                        StructFieldInfo fi;
                        fi.name = f.name;
                        fi.type = ft;
                        fi.offset = bf_offset;
                        fi.size   = bf_size;
                        fi.bit_offset = bf_used;
                        fi.bit_width  = bw;
                        layout.fields.push_back(std::move(fi));
                        bf_used += bw;
                        continue;
                    }
                    // Campo normal: cerrar bit field activo si lo hay.
                    close_bf();

                    // Padding hasta multiplo de falign.
                    if (offset % falign != 0) {
                        offset += falign - (offset % falign);
                    }

                    StructFieldInfo fi;
                    fi.name   = f.name;
                    fi.type   = ft;
                    fi.offset = offset;
                    fi.size   = fsize;
                    layout.fields.push_back(std::move(fi));

                    offset += fsize;
                    if (falign > max_align) max_align = falign;
                }
                // Cerrar bit field activo al final del struct.
                close_bf();
                // Tamano total redondeado al max_align (compatible con
                // arrays de structs).
                if (max_align > 1 && offset % max_align != 0) {
                    offset += max_align - (offset % max_align);
                }
                layout.size_bytes   = offset;
                layout.align_bytes  = max_align;
                /* preservar la marca @Introspect que
                 * la pre-pasada copio del AST.  Como aqui sobrescribimos
                 * la entrada con un layout local fresco, hay que re-copiar
                 * el flag desde el StructDecl. */
                layout.is_introspect = s->is_introspect;
                // Phase M6.a L.3.
                layout.is_public = s->is_public;

                // Sobrescribir la entrada vacia pre-registrada con el layout
                // ya completo.  Usar operator[] = porque la entrada existe.
                struct_layouts_[s->name] = std::move(layout);
            } else if (decl->kind == ast::NodeKind::EnumDecl) {
                // Registrar el enum (ADT) con su layout: max_payload determina
                // el tamano del slot (8 + 8*N_payload_fields_max) y los tags.
                auto *en = static_cast<ast::EnumDecl *>(decl.get());
                // L2.3: enums template (con type_params) NO se procesan como
                // concretos; se monomorphizan on demand.
                if (!en->type_params.empty()) continue;
                // Pre-pasada creo entradas vacias en enum_layouts_; un
                // enum es "ya registrado" si tiene variantes.  Para
                // colisiones cross-tipo, struct/class no deberian
                // existir con el mismo nombre.
                auto it_pre_e = enum_layouts_.find(en->name);
                const bool already_done = (it_pre_e != enum_layouts_.end()
                                          && !it_pre_e->second.variants.empty());
                auto it_struct_done = struct_layouts_.find(en->name);
                auto it_class_done  = class_layouts_.find(en->name);
                const bool struct_collision = (it_struct_done != struct_layouts_.end()
                                              && !it_struct_done->second.fields.empty());
                const bool class_collision  = (it_class_done != class_layouts_.end()
                                              && !it_class_done->second.name.empty()
                                              && it_class_done->second.name == en->name
                                              && (!it_class_done->second.fields.empty()
                                                  || !it_class_done->second.methods.empty()));
                // L2.3: enums monomorphizados ya tienen su layout completo
                // (lo construye monomorphize_enum); skip silente sin error.
                if (monomorphized_.count(en->name)) {
                    continue;
                }
                if (already_done || struct_collision || class_collision) {
                    diags_.error(en->loc,
                        "tipo redeclarado: '" + en->name + "' (colision con struct/class/enum)");
                    continue;
                }
                EnumLayout elay;
                elay.name = en->name;
                std::unordered_map<std::string, bool> seen_v;
                uint32_t max_pl = 0;
                for (size_t vi = 0; vi < en->variants.size(); ++vi) {
                    const auto &vd = en->variants[vi];
                    if (!seen_v.emplace(vd.name, true).second) {
                        diags_.error(vd.loc,
                            "variante duplicada en enum '" + en->name + "': '" + vd.name + "'");
                        continue;
                    }
                    EnumVariantInfo vi_info;
                    vi_info.name = vd.name;
                    vi_info.tag  = static_cast<uint32_t>(vi);
                    vi_info.field_types.reserve(vd.field_types.size());
                    for (const auto &ft : vd.field_types) {
                        vi_info.field_types.push_back(type_from_node(ft.get()));
                    }
                    if (vi_info.field_types.size() > max_pl) {
                        max_pl = static_cast<uint32_t>(vi_info.field_types.size());
                    }
                    elay.variants.push_back(std::move(vi_info));
                }
                elay.max_payload_fields = max_pl;
                // Layout: 8 (tag) + 8 * max_payload_fields.  Cada payload
                // se padea a 8 bytes para uniformidad del offset acceso
                // (i*8 a partir de offset 8) sin tablas por variante.
                elay.size_bytes = 8 + 8 * max_pl;
                /* preservar marca @Introspect del AST. */
                elay.is_introspect = en->is_introspect;
                // Phase M6.a L.3.
                elay.is_public = en->is_public;
                // Sobrescribir entrada vacia pre-registrada.
                enum_layouts_[en->name] = std::move(elay);
            } else if (decl->kind == ast::NodeKind::ClassDecl) {
                auto *c = static_cast<ast::ClassDecl *>(decl.get());
                // generics: templates (con type_params) no son clases
                // concretas.  Solo se procesa la version monomorphizada
                // (que tiene type_params vacio).
                if (!c->type_params.empty()) continue;
                // Pre-pasada creo entradas vacias en class_layouts_; un
                // class es "ya completada" si tiene fields o methods.
                auto it_pre_c = class_layouts_.find(c->name);
                if (it_pre_c != class_layouts_.end()
                 && (!it_pre_c->second.fields.empty()
                     || !it_pre_c->second.methods.empty())) {
                    diags_.error(c->loc, "clase redeclarada: '" + c->name + "'");
                    continue;
                }

                // Layout de campos: cada slot ocupa 8 bytes (igual que el
                // ClassRegistry) por simplicidad y alineacion.  El offset
                // efectivo en el ObjectHeader lo recalcula el ClassRegistry
                // sumandole sizeof(ObjectHeader); aqui guardamos el offset
                // relativo al payload para que el lowering pueda emitir
                // GETFIELD <off>.
                ClassLayout layout;
                layout.name            = c->name;
                layout.super_name      = c->super_name;
                layout.interface_names = c->interface_names;
                layout.is_interface    = c->is_interface;
                /* preservar la marca @Introspect del AST. */
                layout.is_introspect   = c->is_introspect;
                layout.is_aspect       = c->is_aspect;
                // Phase M6.a L.3: visibilidad cross-module.
                layout.is_public       = c->is_public;

                // Herencia: si hay super_name resuelto, copiamos sus fields
                // y metodos al inicio del layout actual.  Los offsets
                // continuan tras los heredados; los slots del vtable se
                // mantienen para que un metodo no override lo mantenga
                // como heredado.  Override por nombre se resuelve mas
                // abajo cuando procesamos los metodos propios.
                const ClassLayout *super_layout = nullptr;
                if (!c->super_name.empty()) {
                    // Phase M.L30: detector de ciclos de herencia.
                    // Recorrer la cadena super_name -> super_name de la
                    // clase candidata.  Si llegamos al name de la clase
                    // que estamos procesando, hay un ciclo (e.g. A:B y
                    // B:A o A:B:C:A).  Limite defensivo de 256 niveles
                    // para evitar loops espurios en layouts mal formados.
                    {
                        const std::string &self_name = c->name;
                        std::string cur = c->super_name;
                        for (int depth = 0; depth < 256; ++depth) {
                            if (cur == self_name) {
                                diags_.error(c->loc,
                                    "ciclo de herencia detectado: la clase '" +
                                    self_name + "' aparece en su propia "
                                    "cadena de superclases");
                                break;
                            }
                            auto it_cycle = class_layouts_.find(cur);
                            if (it_cycle == class_layouts_.end()) break;
                            if (it_cycle->second.super_name.empty()) break;
                            cur = it_cycle->second.super_name;
                        }
                    }
                    auto it_super = class_layouts_.find(c->super_name);
                    if (it_super == class_layouts_.end()) {
                        diags_.error(c->loc,
                            "superclase '" + c->super_name +
                            "' no encontrada (debe declararse antes que '" +
                            c->name + "')");
                    } else if (it_super->second.is_interface
                               && !c->is_interface) {
                        // El identificador despues de `:` resulto ser una
                        // interfaz, no una clase.  Promocionamos a la lista
                        // de interfaces implementadas y vaciamos super_name
                        // (la clase queda sin super, equivalente a Object).
                        // Esto permite la sintaxis natural Vex
                        // `class X : IFoo, IBar` sin requerir un super
                        // dummy en primera posicion.
                        layout.interface_names.insert(
                            layout.interface_names.begin(),
                            c->super_name);
                        layout.super_name.clear();
                    } else {
                        super_layout = &it_super->second;
                        // Copiar fields heredados (offsets ya incluyen header).
                        for (const auto &sf : super_layout->fields) {
                            layout.fields.push_back(sf);
                        }
                        for (const auto &ssf : super_layout->static_fields) {
                            layout.static_fields.push_back(ssf);
                        }
                        // Copiar metodos heredados con sus vtable_index.
                        for (const auto &sm : super_layout->methods) {
                            layout.methods.push_back(sm);
                        }
                        // Marcar cuantos elementos son heredados para que
                        // el lowering de __module_init los omita (los
                        // anyade ya define_class en el loader).
                        layout.inherited_field_count =
                            static_cast<uint32_t>(super_layout->fields.size());
                        layout.inherited_static_field_count =
                            static_cast<uint32_t>(super_layout->static_fields.size());
                    }
                }

                std::unordered_map<std::string, bool> seen_field;
                for (const auto &fi : layout.fields) seen_field[fi.name] = true;
                // Los campos de instancia van DESPUES del ObjectHeader
                // (24 bytes) en el layout en memoria.  El frontend usa
                // estos offsets directamente con xchg cur0,r_obj +
                // addcur, asi que deben incluir el header para apuntar
                // al field real.  Los static_fields parten de 0
                // (offset relativo al bloque static_data).
                uint32_t off_inst = static_cast<uint32_t>(sizeof(loader::ObjectHeader));
                if (super_layout) {
                    // Continuar tras los fields heredados.
                    off_inst += static_cast<uint32_t>(super_layout->fields.size()) * 8;
                }
                uint32_t off_stat = 0;
                for (const auto &f : c->fields) {
                    if (!seen_field.emplace(f.name, true).second) {
                        diags_.error(f.loc,
                            "campo duplicado en clase '" + c->name + "': '" + f.name + "'");
                        continue;
                    }
                    Type ft = type_from_node(f.type.get());
                    if (ft.kind == PrimitiveKind::COUNT) {
                        diags_.error(f.loc,
                            "tipo invalido en campo '" + f.name + "' de la clase '" + c->name + "'");
                        continue;
                    }
                    StructFieldInfo fi;
                    fi.name = f.name;
                    fi.type = ft;
                    fi.size = 8; // todos los slots de instancia ocupan 8 bytes
                    if (f.is_static) {
                        fi.offset = off_stat;
                        off_stat += 8;
                        layout.static_fields.push_back(std::move(fi));
                    } else {
                        fi.offset = off_inst;
                        off_inst += 8;
                        layout.fields.push_back(std::move(fi));
                    }
                }
                layout.size_bytes = off_inst;

                // Resumen de metodos (incluyendo constructor).  Override:
                // si un metodo de la subclase tiene el mismo nombre que
                // uno heredado, REEMPLAZA el slot del super (mismo
                // vtable_index).  El AST lo marca con is_override pero
                // tambien aceptamos override implicito si el nombre coincide.
                std::unordered_map<std::string, bool> seen_method;
                for (const auto &m_inh : layout.methods) seen_method[m_inh.name] = true;
                for (size_t mi = 0; mi < c->methods.size(); ++mi) {
                    const auto *m = c->methods[mi].get();
                    const std::string &mname = m->name;

                    // Detectar override: ya existe un metodo con ese nombre
                    // (heredado del super).  Buscar su slot.
                    int override_idx = -1;
                    if (!m->is_constructor) {
                        for (size_t j = 0; j < layout.methods.size(); ++j) {
                            if (layout.methods[j].name == mname) {
                                override_idx = static_cast<int>(j);
                                break;
                            }
                        }
                    }

                    if (override_idx >= 0) {
                        // Override de metodo heredado.  Validar que el
                        // metodo del super NO es final.
                        if (layout.methods[override_idx].is_final) {
                            diags_.error(m->loc,
                                "no se puede sobrescribir el metodo final '" +
                                mname + "' de la superclase");
                            continue;
                        }
                        // Reemplazar el slot manteniendo vtable_index.
                        ClassMethodInfo mi_info;
                        mi_info.name           = mname;
                        mi_info.is_constructor = false;
                        mi_info.is_static      = m->is_static;
                        mi_info.is_final       = m->is_final;
                        mi_info.is_inline      = m->is_inline;
                        mi_info.defining_class = c->name;  // override en esta clase
                        mi_info.return_type    = m->return_type
                                                  ? type_from_node(m->return_type.get())
                                                  : Type{PrimitiveKind::VOID};
                        mi_info.param_types.reserve(m->params.size());
                        for (const auto &p : m->params) {
                            mi_info.param_types.push_back(type_from_node(p->type.get()));
                        }
                        mi_info.vtable_index = layout.methods[override_idx].vtable_index;
                        layout.methods[override_idx] = std::move(mi_info);
                        continue;
                    }

                    // Metodo nuevo (no override).  Si is_override estaba
                    // marcado pero no hay metodo con ese nombre en la
                    // jerarquia, error.
                    // Bug fix 2026-05-23: @Override tambien valido si
                    // implementa un metodo de cualquier interface declarada
                    // por la clase (interface_names).  Sin esto, el patron
                    // estandar Java/C# `class X : IFoo { @Override foo() }`
                    // se rechazaba con "metodo 'foo' no existe en la jerarquia"
                    // forzando a quitar @Override del codigo.
                    if (m->is_override) {
                        bool found_in_iface = false;
                        // Usar layout.interface_names (con promote `IFoo` desde
                        // super_name aplicada) en lugar de c->interface_names
                        // (que es el AST raw sin promote).
                        for (const auto &iname : layout.interface_names) {
                            auto it_if = class_layouts_.find(iname);
                            if (it_if == class_layouts_.end()) continue;
                            for (const auto &im : it_if->second.methods) {
                                if (im.name == mname) {
                                    found_in_iface = true;
                                    break;
                                }
                            }
                            if (found_in_iface) break;
                        }
                        if (!found_in_iface) {
                            diags_.error(m->loc,
                                "@Override: el metodo '" + mname +
                                "' no existe en la jerarquia de la clase");
                        }
                    }

                    if (!seen_method.emplace(mname, true).second
                     && !m->is_constructor) {
                        diags_.error(m->loc,
                            "metodo duplicado en clase '" + c->name + "': '" + mname + "'");
                        continue;
                    }
                    ClassMethodInfo mi_info;
                    mi_info.name           = mname;
                    mi_info.is_constructor = m->is_constructor;
                    mi_info.is_destructor  = m->is_destructor;
                    mi_info.is_static      = m->is_static;
                    mi_info.is_final       = m->is_final;
                    mi_info.is_inline      = m->is_inline;
                    mi_info.defining_class = c->name;
                    // capturar source file + line del decl.
                    mi_info.source_file    = m->loc.file;
                    mi_info.source_line    = m->loc.line;
                    mi_info.return_type    = m->return_type
                                              ? type_from_node(m->return_type.get())
                                              : Type{PrimitiveKind::VOID};
                    mi_info.param_types.reserve(m->params.size());
                    for (const auto &p : m->params) {
                        mi_info.param_types.push_back(type_from_node(p->type.get()));
                    }
                    mi_info.vtable_index = static_cast<uint32_t>(layout.methods.size());

                    // fix12 - detectar ctores trivial zero-init.  Si el
                    // body es solo `this.field = 0|0.0|null|false` para varios
                    // fields, podemos saltar la callvirt al ctor en runtime
                    // porque el GC ya hace memset a 0 del payload.  Solo aplica
                    // si la clase NO tiene super custom (super == "Object" o
                    // ausente): si hay super con ctor no-trivial, hay que llamarlo.
                    if (m->is_constructor && m->body
                        && (c->super_name.empty() || c->super_name == "Object")) {
                        bool zero_init_only = true;
                        for (const auto &stmt : m->body->body) {
                            if (!stmt) { zero_init_only = false; break; }
                            // Aceptamos solo ExprStmt con AssignExpr de la forma
                            // <FieldAccess>.field = literal_zero
                            if (stmt->kind != ast::NodeKind::ExprStmt) {
                                zero_init_only = false; break;
                            }
                            auto *es = static_cast<ast::ExprStmt *>(stmt.get());
                            if (!es->expr || es->expr->kind != ast::NodeKind::AssignExpr) {
                                zero_init_only = false; break;
                            }
                            auto *ae = static_cast<ast::AssignExpr *>(es->expr.get());
                            if (ae->op != ast::AssignOp::Assign) {
                                zero_init_only = false; break;
                            }
                            // target: FieldAccessExpr cuyo base es ThisExpr.
                            if (!ae->target
                             || ae->target->kind != ast::NodeKind::FieldAccessExpr) {
                                zero_init_only = false; break;
                            }
                            auto *fa = static_cast<ast::FieldAccessExpr *>(ae->target.get());
                            if (!fa->base || fa->base->kind != ast::NodeKind::ThisExpr) {
                                zero_init_only = false; break;
                            }
                            // value: literal cero/false/null.
                            if (!ae->value) { zero_init_only = false; break; }
                            const ast::Expr *v = ae->value.get();
                            bool is_zero_lit = false;
                            if (v->kind == ast::NodeKind::IntLitExpr) {
                                is_zero_lit = (static_cast<const ast::IntLitExpr *>(v)->value == 0);
                            } else if (v->kind == ast::NodeKind::FloatLitExpr) {
                                is_zero_lit = (static_cast<const ast::FloatLitExpr *>(v)->value == 0.0);
                            } else if (v->kind == ast::NodeKind::BoolLitExpr) {
                                is_zero_lit = (static_cast<const ast::BoolLitExpr *>(v)->value == false);
                            } else if (v->kind == ast::NodeKind::NullLitExpr) {
                                is_zero_lit = true;
                            }
                            if (!is_zero_lit) { zero_init_only = false; break; }
                        }
                        // Body vacio tambien cuenta como zero-init trivial.
                        mi_info.is_zero_init_ctor = zero_init_only;
                    }

                    layout.methods.push_back(std::move(mi_info));
                }

                // precomputar has_destructor para que las rules de
                // escape (check_assign) lo consulten en O(1) sin iterar
                // metodos.  Importante: heredamos del super, asi que si la
                // clase no declara su propio dtor pero el super si lo tiene,
                // se considera destructible (el dtor del super correra).
                for (const auto &mi : layout.methods) {
                    if (mi.is_destructor) { layout.has_destructor = true; break; }
                }

                // Sobrescribir entrada vacia pre-registrada con el layout
                // ya completo.
                class_layouts_[c->name] = std::move(layout);
            }
        }

        // punto-fijo de @c has_destructible_field.  Una clase tiene
        // @c has_destructible_field si alguno de sus fields (incluidos los
        // heredados del super) es de tipo CLASS y esa clase tiene
        // @c has_destructor o @c has_destructible_field a su vez.
        //
        // Iteramos hasta estabilizar para soportar referencias mutuamente
        // recursivas (LinkedList { Node head; } / Node { Node next; }).
        // En cada iteracion, si una clase X gana @c has_destructible_field,
        // las clases que la contienen como field tambien lo ganan.  Como
        // efecto del cierre transitivo, cualquier ciclo se resuelve en N
        // iteraciones donde N es la profundidad maxima de la cadena.
        for (bool changed = true; changed; ) {
            changed = false;
            for (auto &kv : class_layouts_) {
                ClassLayout &cl = kv.second;
                if (cl.is_interface || cl.is_runtime_predefined) continue;
                if (cl.has_destructible_field) continue;  // ya maximo
                for (const auto &f : cl.fields) {
                    // Solo fields de tipo CLASS aportan destructibilidad.
                    if (f.type.kind != PrimitiveKind::CLASS) continue;
                    auto it_inner = class_layouts_.find(f.type.struct_name);
                    if (it_inner == class_layouts_.end()) continue;
                    const ClassLayout &inner = it_inner->second;
                    if (inner.has_destructor || inner.has_destructible_field) {
                        cl.has_destructible_field = true;
                        changed = true;
                        break;
                    }
                }
            }
        }

        // sintesis del destructor implicito.  Para cada clase con
        // @c has_destructible_field == true y SIN destructor declarado por
        // el usuario, anadimos un ClassMethodDecl sintetico con body vacio
        // y @c is_destructor = true.  El lowering augmenta el body de TODOS
        // los destructores (sintetizados o no) con CALLVIRT a los dtors de
        // los fields destructibles, asi que un cuerpo vacio basta para
        // disparar la cadena RAII recursiva.
        //
        // Tras la sintesis actualizamos @c has_destructor a true (el flag
        // representa la presencia EFECTIVA de un destructor invocable, sea
        // user o sintetico).  Esto unifica todas las consultas downstream.
        for (auto &mod_node : mod_.decls) {
            if (!mod_node || mod_node->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(mod_node.get());
            auto it_lay = class_layouts_.find(cd->name);
            if (it_lay == class_layouts_.end()) continue;
            ClassLayout &lay = it_lay->second;
            if (lay.is_interface || lay.is_runtime_predefined) continue;
            if (!lay.has_destructible_field) continue;
            if (lay.has_destructor) continue;  // user ya declaro uno

            // Anadir ClassMethodDecl sintetica al AST con body vacio.
            auto dtor = std::make_unique<ast::ClassMethodDecl>();
            dtor->loc           = cd->loc;
            dtor->name          = "__dtor";
            dtor->is_destructor = true;
            dtor->access        = 0;
            dtor->body          = std::make_unique<ast::BlockStmt>();
            dtor->body->loc     = cd->loc;
            cd->methods.push_back(std::move(dtor));

            // Reflejarlo en el ClassLayout: anadir un ClassMethodInfo y
            // marcar @c has_destructor.  El @c vtable_index sigue al final
            // de los metodos existentes.
            ClassMethodInfo mi_info;
            mi_info.name           = "__dtor";
            mi_info.is_destructor  = true;
            mi_info.is_constructor = false;
            mi_info.is_static      = false;
            mi_info.is_final       = false;
            mi_info.is_inline      = false;
            mi_info.defining_class = cd->name;
            mi_info.return_type    = Type{PrimitiveKind::VOID};
            mi_info.vtable_index   = static_cast<uint32_t>(lay.methods.size());
            mi_info.source_file    = cd->loc.file;
            mi_info.source_line    = cd->loc.line;
            lay.methods.push_back(std::move(mi_info));
            lay.has_destructor = true;
        }

        // -----------------------------------------------------------------
        // Validacion de implementacion de interfaces.
        //
        // Para cada clase NO-interface con `class X : Base, IFoo, IBar`,
        // verificar que todos los metodos abstractos de IFoo y IBar
        // (incluyendo los heredados por sus super-interfaces) estan
        // presentes en X (o en algun ancestro) con firma compatible.
        // Las interfaces que no encuentre se reportan como error claro.
        // -----------------------------------------------------------------
        for (auto &kv : class_layouts_) {
            const ClassLayout &cl = kv.second;
            if (cl.is_interface) continue;        // las interfaces no implementan otras
            for (const std::string &iname : cl.interface_names) {
                auto it_iface = class_layouts_.find(iname);
                if (it_iface == class_layouts_.end()) {
                    diags_.error(SourceLoc{},
                        "interfaz '" + iname + "' usada por '" + cl.name +
                        "' no esta declarada");
                    continue;
                }
                if (!it_iface->second.is_interface) {
                    diags_.error(SourceLoc{},
                        "'" + iname + "' usada como interfaz por '" + cl.name +
                        "' es una clase, no una interfaz");
                    continue;
                }
                // Verificar que cada metodo abstracto de la interfaz esta
                // implementado en cl (mismo nombre + aridad + tipos).
                const ClassLayout &iface = it_iface->second;
                for (const ClassMethodInfo &im : iface.methods) {
                    bool found = false;
                    for (const ClassMethodInfo &cm : cl.methods) {
                        if (cm.name != im.name) continue;
                        if (cm.param_types.size() != im.param_types.size()) continue;
                        // Comparacion shallow de firmas: kinds primarios.
                        bool sigs_ok = (cm.return_type.kind == im.return_type.kind);
                        for (size_t i = 0;
                             sigs_ok && i < cm.param_types.size(); ++i) {
                            if (cm.param_types[i].kind != im.param_types[i].kind) {
                                sigs_ok = false;
                            }
                        }
                        if (sigs_ok) { found = true; break; }
                    }
                    if (!found) {
                        diags_.error(SourceLoc{},
                            "la clase '" + cl.name + "' no implementa el metodo '" +
                            im.name + "' requerido por la interfaz '" + iname + "'");
                    }
                }
            }
        }

        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::FunctionDecl) {
                auto *fn = static_cast<ast::FunctionDecl *>(decl.get());
                /* comptime fn -- registrar para que las llamadas
                 * desde contextos comptime puedan interpretar el body.
                 * NO se registra como Symbol::Function regular porque
                 * NO debe llamarse desde codigo runtime.  Pero sigue
                 * declarando el nombre en el scope global para que el
                 * type checker resuelva el ident en el callee de las
                 * llamadas (comptime_eval_expr discrimina luego). */
                if (fn->is_comptime) {
                    register_comptime_fn(fn->name, fn);
                    /* Tambien registramos un Symbol::Function dummy para
                     * que `lookup(fn->name)` lo resuelva.  El sig real
                     * importa poco porque la llamada nunca sale a IR. */
                    FunctionSig sig_ct;
                    sig_ct.return_type = type_from_node(fn->return_type.get());
                    sig_ct.param_types.reserve(fn->params.size());
                    for (auto &p : fn->params) {
                        sig_ct.param_types.push_back(type_from_node(p->type.get()));
                    }
                    Symbol s;
                    s.kind      = SymbolKind::Function;
                    s.sig_index = (uint32_t)function_sigs_.size();
                    sig_by_name_[fn->name] = s.sig_index;
                    function_sigs_.push_back(std::move(sig_ct));
                    if (!declare(fn->name, s)) {
                        diags_.error(fn->loc,
                            "comptime fn: redefinicion de '" + fn->name + "'");
                    }
                    continue;
                }

                FunctionSig sig;
                Type ret_t = type_from_node(fn->return_type.get());
                // Mejora II: si la funcion es @Async, el wrapper publico
                // visible al callsite devuelve Future<T> donde T es el tipo
                // declarado del @c return.  El bytecode del wrapper sigue
                // produciendo un i64 (handle), pero al frontend le
                // interesa preservar T para que `T r = await fn();` se
                // tipo-checkee correctamente.
                if (fn->is_async) {
                    sig.return_type = Type::make_future(std::move(ret_t));
                } else {
                    sig.return_type = std::move(ret_t);
                }
                sig.param_types.reserve(fn->params.size());
                for (auto &p : fn->params) {
                    sig.param_types.push_back(type_from_node(p->type.get()));
                }

                Symbol s;
                s.kind      = SymbolKind::Function;
                s.sig_index = (uint32_t)function_sigs_.size();
                sig_by_name_[fn->name] = s.sig_index;
                function_sigs_.push_back(std::move(sig));
                if (!declare(fn->name, s)) {
                    // Bug fix 2026-05-23: forward declaration -- si el simbolo
                    // ya existe Y este es un forward decl (sin body), OK.
                    // Si la PREVIA era forward y esta tiene body, tambien OK
                    // (es la definicion completando la forward).  Solo error
                    // si AMBAS tienen body (redefinicion real).
                    if (!fn->is_forward_decl) {
                        // Verificar si el simbolo existente era un forward decl.
                        const Symbol *prev = lookup(fn->name);
                        bool prev_is_forward = false;
                        if (prev && prev->kind == SymbolKind::Function) {
                            for (auto &d2 : mod_.decls) {
                                if (!d2 || d2->kind != ast::NodeKind::FunctionDecl) continue;
                                auto *prev_fn = static_cast<ast::FunctionDecl *>(d2.get());
                                if (prev_fn != fn && prev_fn->name == fn->name
                                 && prev_fn->is_forward_decl) {
                                    prev_is_forward = true; break;
                                }
                            }
                        }
                        if (!prev_is_forward) {
                            diags_.error(fn->loc,
                                "redefinicion de simbolo a nivel global: '" + fn->name + "'");
                        }
                    }
                }
            } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
                auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                Symbol s;
                s.kind     = SymbolKind::Variable;
                s.type     = type_from_node(gv->type.get());
                s.is_const = gv->is_const;
                if (!declare(gv->name, s)) {
                    diags_.error(gv->loc, "redefinicion de simbolo a nivel global: '" + gv->name + "'");
                }
            } else if (decl->kind == ast::NodeKind::ExternFnDecl) {
                // FFI declarativo: registrar como Symbol::Function con
                // FunctionSig::extern_lib != "" para que el lowering emita
                // CALLN @Method("<lib>:<name>") en vez de CALLVM al llamarla.
                auto *efd = static_cast<ast::ExternFnDecl *>(decl.get());
                FunctionSig sig;
                sig.return_type = type_from_node(efd->return_type.get());
                sig.param_types.reserve(efd->params.size());
                for (auto &p : efd->params) {
                    sig.param_types.push_back(type_from_node(p->type.get()));
                }
                sig.extern_lib = efd->lib;
                Symbol s;
                s.kind      = SymbolKind::Function;
                s.sig_index = (uint32_t)function_sigs_.size();
                sig_by_name_[efd->name] = s.sig_index;
                function_sigs_.push_back(std::move(sig));
                if (!declare(efd->name, s)) {
                    diags_.error(efd->loc,
                        "redefinicion de simbolo extern: '" + efd->name + "'");
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Pase 2: cuerpos de funciones.
    // ---------------------------------------------------------------------

    void TypeChecker::check_functions() {
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::FunctionDecl) {
                // Globales: si tienen init, chequear el tipo.
                if (decl && decl->kind == ast::NodeKind::GlobalVarDecl) {
                    auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                    if (gv->init) {
                        Type t = check_expr(gv->init.get());
                        const Type want = type_from_node(gv->type.get());
                        /* A.39: para comptime const NO aplicamos el check
                         * estricto -- el "tipo" del literal es flexible
                         * (string literal tiene type PTR pero comptime
                         * lo evalua como STRING).  La validez se chequea
                         * via comptime_eval_expr abajo. */
                        if (gv->is_comptime) {
                            /* skip */
                        } else
                        if (t.kind != PrimitiveKind::COUNT
                         && want.kind != PrimitiveKind::COUNT
                         && !is_numeric(t.kind) == !is_numeric(want.kind)
                         && t != want)
                        {
                            // Si ambos son numericos pero no exactamente iguales,
                            // toleramos la promocion implicita (la decision final
                            // la toma el lowering).  Si no son ambos numericos,
                            // exigimos igualdad estricta.
                            // Bug fix 2026-05-23: tolerar `string g = "lit"` --
                            // el init es PTR (literal raw) y el destino STRING;
                            // el lowering hara la promotion via STRMAKE en
                            // __module_init (igual que vars locales).
                            const bool is_str_from_lit =
                                want.kind == PrimitiveKind::STRING
                                && t.kind == PrimitiveKind::PTR
                                && gv->init
                                && gv->init->kind == ast::NodeKind::StringLitExpr;
                            if (!(is_numeric(t.kind) && is_numeric(want.kind))
                                && !is_str_from_lit) {
                                diags_.error(gv->loc,
                                    std::string("tipo del inicializador (") + primitive_name(t.kind) +
                                    ") incompatible con el tipo declarado (" + primitive_name(want.kind) + ")");
                            }
                        }
                    }
                    /* comptime const NAME = expr; evaluar init en
                     * compile-time y cachear el valor.  Errors si no es
                     * comptime-evaluable.  LANG.fix-2: si el global YA
                     * fue inicializado (por init_comptime_globals previo
                     * que llama el runtime de top-level comptime block),
                     * NO re-evaluar el init -- preservamos las mutaciones
                     * aplicadas por el bloque. */
                    if (gv->is_comptime
                     && comptime_const_values_.find(gv->name)
                            == comptime_const_values_.end()) {
                        if (!gv->init) {
                            diags_.error(gv->loc,
                                "'comptime const " + gv->name +
                                "' requiere un inicializador");
                        } else {
                            const ComptimeEvalResult r =
                                comptime_eval_expr(*this, gv->init.get());
                            if (!r.ok) {
                                diags_.error(gv->init->loc,
                                    "el init de 'comptime const " + gv->name +
                                    "' no es comptime-evaluable");
                            } else {
                                ComptimeConst c;
                                /* sugar: si gv->type es nullptr
                                 * (sugar `comptime X = ...` sin tipo
                                 * explicito), inferimos el tipo desde el
                                 * ComptimeEvalResult: is_str->STRING,
                                 * is_type->TYPE_META, else->I64. */
                                if (gv->type) {
                                    c.type = type_from_node(gv->type.get());
                                } else if (r.is_str) {
                                    c.type = Type{PrimitiveKind::STRING};
                                } else if (r.is_type) {
                                    c.type = Type{PrimitiveKind::TYPE_META};
                                } else {
                                    c.type = Type{PrimitiveKind::I64};
                                }
                                c.is_str    = r.is_str;
                                c.is_array  = r.is_array;
                                c.is_struct = r.is_struct;
                                c.is_type   = r.is_type;
                                /* A.43.17: globales `comptime var` son mutables.
                                 * Los `comptime const` mantienen is_mutable=false.
                                 * Las @Macro y comptime fn pueden modificar
                                 * globals mutables via apply_comptime_assign. */
                                c.is_mutable = !gv->is_const;
                                if (r.is_str)         c.str_value     = r.str;
                                else if (r.is_array)  c.array_vals    = r.array_vals;
                                else if (r.is_struct) c.struct_fields = r.struct_fields;
                                else if (r.is_type)   c.type_val      = r.type_val;
                                else                  c.value         = r.value;
                                // v4: propagar atributos @align/@hot/@cold/@section.
                                c.attr_align   = gv->attr_align;
                                c.attr_hot     = gv->attr_hot;
                                c.attr_cold    = gv->attr_cold;
                                c.attr_section = gv->attr_section;
                                comptime_const_values_[gv->name] = c;
                            }
                        }
                    }
                }
                continue;
            }
            auto *fn = static_cast<ast::FunctionDecl *>(decl.get());
            if (!fn->body) continue;
            /* comptime fn -- el body solo se interpreta al
             * call site via comptime_eval_stmt.  No type-check estatico
             * aqui (los IdentExpr no necesitan annotation; el eval los
             * busca en tc.comptime_const_locals_).  Si hay errores de
             * sintaxis o expresiones invalidas, el eval fallara con
             * `ok=false` en el call y emitiremos error alli. */
            if (fn->is_comptime) continue;

            // Mejora II: validacion @Async extendida.  Antes solo permitia
            // funciones sin parametros y return type i64.  Ahora:
            //   - Cualquier numero de parametros, cada uno con tipo de
            //     tamano <= 8 bytes (primitivos numericos, bool, char,
            //     ptr, handle de string/objeto, futures).
            //   - Cualquier tipo de retorno T con tamano <= 8 bytes.
            //   - El wrapper publico visible al callsite devuelve
            //     Future<T> (envuelto automaticamente por el type checker).
            //     `i32 r = await compute(10, 20);` tipo-checkea correctamente.
            //
            // Tipos > 8 bytes (struct, array, optional, result) requeririan
            // serializacion en buffer auxiliar.  Deferido a Phase B.
            if (fn->is_async) {
                auto fits_in_qword = [](const Type &t) -> bool {
                    if (t.kind == PrimitiveKind::COUNT) return true; // tipo desconocido OK
                    return primitive_size_bytes(t.kind) > 0
                        && primitive_size_bytes(t.kind) <= 8;
                };
                for (size_t pi = 0; pi < fn->params.size(); ++pi) {
                    Type pt = type_from_node(fn->params[pi]->type.get());
                    if (!fits_in_qword(pt)) {
                        diags_.error(fn->params[pi]->loc,
                            "@Async: parametro '" + fn->params[pi]->name +
                            "' de tipo '" + type_to_string(pt) +
                            "' excede 8 bytes.  Tipos compuestos (struct, array, "
                            "Optional, Result) no soportados aun.");
                    }
                }
                const Type rt_chk = type_from_node(fn->return_type.get());
                if (rt_chk.kind != PrimitiveKind::COUNT
                 && rt_chk.kind != PrimitiveKind::VOID
                 && !fits_in_qword(rt_chk)) {
                    diags_.error(fn->loc,
                        "@Async: tipo de retorno '" + type_to_string(rt_chk) +
                        "' excede 8 bytes.  Tipos compuestos no soportados aun.");
                }
            }

            const Type fn_ret = type_from_node(fn->return_type.get());
            push_scope(); // scope de la funcion (parametros)
            for (auto &p : fn->params) {
                Symbol sp;
                sp.kind = SymbolKind::Param;
                sp.type = type_from_node(p->type.get());
                if (!declare(p->name, sp)) {
                    diags_.error(p->loc, "parametro repetido: '" + p->name + "'");
                }
            }
            // Guardar y settear current_fn_return_type_ para que
            // check_match (que recibe expected_return_type via miembro
            // y no via param para no contaminar la signature) pueda
            // validar returns dentro del match con el tipo correcto.
            const Type saved_ret = current_fn_return_type_;
            current_fn_return_type_ = fn_ret;
            // Resetear el borrow checker al entrar a cada funcion.
            // Cada funcion tiene su propio scope de borrows; los borrows
            // de una funcion no afectan a otra.
            borrow_checker_.reset();
            // F2: registrar parametros como owners con OwnerKind::Param.
            // Si el parametro ES un borrow (su tipo es BORROW/BORROW_MUT),
            // ademas lo registramos como borrower self-referencial: el
            // borrow checker lo reconoce como "borrow valido cuyo owner
            // es Param" -> escape via return permitido.
            for (auto &p : fn->params) {
                borrow_checker_.declare_owner(p->name, OwnerKind::Param);
                const Type pt = type_from_node(p->type.get());
                if (pt.kind == PrimitiveKind::BORROW
                 || pt.kind == PrimitiveKind::BORROW_MUT) {
                    borrow_checker_.register_borrow(
                        p->name, p->name,
                        /*is_mut=*/(pt.kind == PrimitiveKind::BORROW_MUT));
                }
            }
            // F1 NLL: pre-pase de last-uses + reset del contador.
            current_stmt_idx_ = 0;
            compute_borrow_last_uses(fn->body.get());
            // BugFix R8: cuando estamos dentro de un @Macro body, todas
            // las var-decls se tratan automaticamente como comptime
            // const (incluso si no llevan `comptime` explicito).  Esto
            // permite que `string n = typename<T>(); comptime_concat(n, "/")`
            // funcione naturalmente sin que el usuario tenga que escribir
            // `comptime const string n = ...`.
            const bool saved_is_macro = current_fn_is_macro_;
            current_fn_is_macro_ = fn->is_macro;
            // Tambien empujamos un scope comptime nuevo para los locals del
            // macro body (para que find_comptime_local_mut los encuentre).
            if (fn->is_macro) push_comptime_scope();
            check_block(fn->body.get(), fn_ret);
            if (fn->is_macro) pop_comptime_scope();
            current_fn_is_macro_ = saved_is_macro;
            current_fn_return_type_ = saved_ret;
            pop_scope();
        }

        // Chequeo de cuerpos de metodos de clase.  Para cada ClassDecl
        // recorremos sus metodos: el scope local incluye 'this' y los
        // parametros declarados.  El campo current_class_ guia
        // check_this para que sepa la clase contenedora.
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            auto it = class_layouts_.find(cd->name);
            if (it == class_layouts_.end()) continue;
            const ClassLayout &cls = it->second;

            const std::string saved_class = current_class_;
            current_class_ = cd->name;

            for (auto &m : cd->methods) {
                if (!m || !m->body) continue;
                check_class_method(cls, m.get());
            }
            current_class_ = saved_class;
        }
    }

    /**
     * @brief Sprint edge-bugs (2026-06-02): rewrite implicit-this.
     *
     * En metodos de instancia, identifiers como @c v dentro del body que
     * matchean un field name de la clase contenedora se reescriben como
     * @c this.v (FieldAccessExpr).  Convencion Java/C#.  No reescribe si:
     *   - el ident matchea un parametro (params_set)
     *   - el ident matchea una local declarada en un scope previo (locals_stack)
     *   - es lhs de un VarDecl (la propia declaracion)
     *
     * El walker recibe @c unique_ptr<Expr>& para poder swap-ear el nodo
     * IdentExpr por FieldAccessExpr in-place.  Walkea TODAS las statements
     * y exprs hijas (recursivo).
     */
    static void rewrite_implicit_this(
        std::unique_ptr<ast::Stmt> &stmt,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set);

    static void rewrite_implicit_this_expr(
        std::unique_ptr<ast::Expr> &node,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set,
        std::vector<std::unordered_set<std::string>> &locals_stack) {
        if (!node) return;
        // IdentExpr: si matches field y NO en params/locals, rewrite.
        if (node->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(node.get());
            if (field_names.count(id->name)
             && !params_set.count(id->name)
             && id->name != "this" && id->name != "super") {
                bool shadowed = false;
                for (auto &s : locals_stack) if (s.count(id->name)) { shadowed = true; break; }
                if (!shadowed) {
                    auto fa = std::make_unique<ast::FieldAccessExpr>();
                    fa->loc = id->loc;
                    auto base = std::make_unique<ast::IdentExpr>();
                    base->loc  = id->loc;
                    base->name = "this";
                    fa->base       = std::move(base);
                    fa->field_name = id->name;
                    node = std::move(fa);
                }
            }
            return;
        }
        switch (node->kind) {
            case ast::NodeKind::BinaryExpr: {
                auto *b = static_cast<ast::BinaryExpr *>(node.get());
                rewrite_implicit_this_expr(b->lhs, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(b->rhs, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *u = static_cast<ast::UnaryExpr *>(node.get());
                rewrite_implicit_this_expr(u->operand, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::TernaryExpr: {
                auto *t = static_cast<ast::TernaryExpr *>(node.get());
                rewrite_implicit_this_expr(t->cond, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(t->then_expr, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(t->else_expr, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::CallExpr: {
                auto *c = static_cast<ast::CallExpr *>(node.get());
                // Para el callee: si es IdentExpr (llamada simple `foo(x)`),
                // NO reescribimos a this.foo -- la llamada se resuelve por
                // simbolo global (clase methods se invocan con this. explicito
                // o se inlinean).  Si es FieldAccessExpr o algo mas, recurse.
                if (c->callee && c->callee->kind != ast::NodeKind::IdentExpr) {
                    rewrite_implicit_this_expr(c->callee, field_names, params_set, locals_stack);
                }
                for (auto &a : c->args)
                    rewrite_implicit_this_expr(a, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::FieldAccessExpr: {
                auto *fa = static_cast<ast::FieldAccessExpr *>(node.get());
                rewrite_implicit_this_expr(fa->base, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::IndexExpr: {
                auto *ix = static_cast<ast::IndexExpr *>(node.get());
                rewrite_implicit_this_expr(ix->base, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(ix->index, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::AssignExpr: {
                auto *a = static_cast<ast::AssignExpr *>(node.get());
                rewrite_implicit_this_expr(a->target, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(a->value, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::CastExpr: {
                auto *c = static_cast<ast::CastExpr *>(node.get());
                rewrite_implicit_this_expr(c->operand, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::NewExpr: {
                auto *n = static_cast<ast::NewExpr *>(node.get());
                for (auto &a : n->args)
                    rewrite_implicit_this_expr(a, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::LambdaExpr: {
                auto *lam = static_cast<ast::LambdaExpr *>(node.get());
                std::unordered_set<std::string> lam_locals;
                for (auto &p : lam->params) lam_locals.insert(p->name);
                locals_stack.push_back(std::move(lam_locals));
                if (lam->body)
                    for (auto &child : lam->body->body)
                        rewrite_implicit_this(child, field_names, params_set);
                locals_stack.pop_back();
                return;
            }
            case ast::NodeKind::StringLitExpr: {
                auto *sl = static_cast<ast::StringLitExpr *>(node.get());
                for (auto &ie : sl->interp_exprs)
                    rewrite_implicit_this_expr(ie, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::MatchExpr: {
                auto *me = static_cast<ast::MatchExpr *>(node.get());
                rewrite_implicit_this_expr(me->scrutinee, field_names, params_set, locals_stack);
                for (auto &arm : me->arms) {
                    std::unordered_set<std::string> arm_locals;
                    for (auto &bn : arm.bindings) arm_locals.insert(bn);
                    locals_stack.push_back(std::move(arm_locals));
                    if (arm.body)
                        rewrite_implicit_this(arm.body, field_names, params_set);
                    locals_stack.pop_back();
                }
                return;
            }
            case ast::NodeKind::InitListExpr: {
                auto *il = static_cast<ast::InitListExpr *>(node.get());
                for (auto &e : il->elements)
                    rewrite_implicit_this_expr(e, field_names, params_set, locals_stack);
                return;
            }
            default:
                return;
        }
    }

    static void rewrite_implicit_this(
        std::unique_ptr<ast::Stmt> &stmt,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set) {
        if (!stmt) return;
        /* Stack de scopes locales (a parte de los params).  Cada BlockStmt
         * push/pop su scope para soportar shadowing.  El VarDecl agrega el
         * nombre al scope ACTUAL antes de procesar las stmts siguientes. */
        std::vector<std::unordered_set<std::string>> locals_stack;
        std::function<void(std::unique_ptr<ast::Stmt> &)> visit;
        visit = [&](std::unique_ptr<ast::Stmt> &s) {
            if (!s) return;
            switch (s->kind) {
                case ast::NodeKind::BlockStmt: {
                    auto *bs = static_cast<ast::BlockStmt *>(s.get());
                    locals_stack.push_back({});
                    for (auto &child : bs->body) visit(child);
                    locals_stack.pop_back();
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<ast::VarDeclStmt *>(s.get());
                    /* Reescribir el init ANTES de declarar el local (el init
                     * no puede referirse a si mismo). */
                    if (vd->init)
                        rewrite_implicit_this_expr(vd->init, field_names,
                            params_set, locals_stack);
                    /* Anadir el nombre al scope local actual.  Si stack vacio,
                     * crear scope top-level. */
                    if (locals_stack.empty()) locals_stack.push_back({});
                    locals_stack.back().insert(vd->name);
                    return;
                }
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<ast::ExprStmt *>(s.get());
                    rewrite_implicit_this_expr(es->expr, field_names,
                        params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *rs = static_cast<ast::ReturnStmt *>(s.get());
                    if (rs->value)
                        rewrite_implicit_this_expr(rs->value, field_names,
                            params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *is = static_cast<ast::IfStmt *>(s.get());
                    rewrite_implicit_this_expr(is->cond, field_names,
                        params_set, locals_stack);
                    visit(is->then_branch);
                    visit(is->else_branch);
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *ws = static_cast<ast::WhileStmt *>(s.get());
                    rewrite_implicit_this_expr(ws->cond, field_names,
                        params_set, locals_stack);
                    visit(ws->body);
                    return;
                }
                case ast::NodeKind::DoWhileStmt: {
                    auto *ds = static_cast<ast::DoWhileStmt *>(s.get());
                    visit(ds->body);
                    rewrite_implicit_this_expr(ds->cond, field_names,
                        params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *fs = static_cast<ast::ForStmt *>(s.get());
                    locals_stack.push_back({});
                    visit(fs->init);
                    if (fs->cond)
                        rewrite_implicit_this_expr(fs->cond, field_names,
                            params_set, locals_stack);
                    if (fs->step)
                        rewrite_implicit_this_expr(fs->step, field_names,
                            params_set, locals_stack);
                    visit(fs->body);
                    locals_stack.pop_back();
                    return;
                }
                case ast::NodeKind::TryStmt: {
                    auto *ts = static_cast<ast::TryStmt *>(s.get());
                    /* body es BlockStmt; iterar children. */
                    if (ts->body)
                        for (auto &child : ts->body->body)
                            visit(child);
                    for (auto &c : ts->catches) {
                        locals_stack.push_back({});
                        if (!c.var_name.empty())
                            locals_stack.back().insert(c.var_name);
                        if (c.body)
                            for (auto &child : c.body->body) visit(child);
                        locals_stack.pop_back();
                    }
                    if (ts->finally_body)
                        for (auto &child : ts->finally_body->body) visit(child);
                    return;
                }
                case ast::NodeKind::ThrowStmt: {
                    auto *th = static_cast<ast::ThrowStmt *>(s.get());
                    if (th->value)
                        rewrite_implicit_this_expr(th->value, field_names,
                            params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::SynchronizedStmt: {
                    auto *ss = static_cast<ast::SynchronizedStmt *>(s.get());
                    rewrite_implicit_this_expr(ss->target, field_names,
                        params_set, locals_stack);
                    if (ss->body)
                        for (auto &child : ss->body->body) visit(child);
                    return;
                }
                default:
                    return;
            }
        };
        visit(stmt);
    }

    void TypeChecker::check_class_method(const ClassLayout &cls, ast::ClassMethodDecl *m) {
        // Semantica de modificadores:
        //  - constructor static: error (no tiene sentido).
        //  - metodo static que use 'this': error claro via check_this
        //    consultando current_method_is_static_.
        //  - final: aceptado, sera enforced en cuando haya override.
        if (m->is_static && m->is_constructor) {
            diags_.error(m->loc,
                "el constructor de '" + cls.name + "' no puede ser 'static'");
        }

        const bool saved_static = current_method_is_static_;
        current_method_is_static_ = m->is_static;

        push_scope();
        // 'this' implicito como Symbol solo en metodos de instancia.
        if (!m->is_static) {
            Symbol s_this;
            s_this.kind = SymbolKind::Param;
            s_this.type = Type{PrimitiveKind::CLASS, cls.name};
            (void)declare("this", s_this);
        }
        // Parametros declarados.
        for (auto &p : m->params) {
            Symbol sp;
            sp.kind = SymbolKind::Param;
            sp.type = type_from_node(p->type.get());
            if (!declare(p->name, sp)) {
                diags_.error(p->loc, "parametro repetido: '" + p->name + "'");
            }
        }
        // Sprint edge-bugs (2026-06-02): pre-pase de implicit-this.
        // Recolecta field names de la clase + sus supers transitivos.
        // Despues walkea el body y reescribe identifiers de fields a
        // FieldAccessExpr(this, field).  Solo para metodos de instancia
        // (en static methods no hay this, debe ser explicito error).
        if (!m->is_static && m->body) {
            std::unordered_set<std::string> field_names;
            const ClassLayout *cl_cur = &cls;
            int depth_guard = 256;
            while (cl_cur && depth_guard-- > 0) {
                for (const auto &f : cl_cur->fields) field_names.insert(f.name);
                if (cl_cur->super_name.empty()) break;
                auto sup_it = class_layouts_.find(cl_cur->super_name);
                if (sup_it == class_layouts_.end()) break;
                cl_cur = &sup_it->second;
            }
            std::unordered_set<std::string> params_set;
            params_set.insert("this");
            params_set.insert("super");
            for (auto &p : m->params) params_set.insert(p->name);
            /* Rewrite inline las stmts del body.  m->body es
             * unique_ptr<BlockStmt> con vector body de unique_ptr<Stmt>.
             *
             * BugFix (2026-06-04): antes iterabamos los children uno a uno
             * llamando @c rewrite_implicit_this(child, ...) , pero esa
             * funcion crea su PROPIO @c locals_stack vacio internamente, lo
             * que provocaba que las var locals declaradas en un statement
             * (e.g. `i64 fp = ffi_call(...)`) NO se vieran como shadowed en
             * el siguiente statement (e.g. `if (fp == 0)`).  Resultado: el
             * `fp` se reescribia a `this.fp` cuando el ident colisionaba
             * con un field, leyendo basura.  Caso real cerrado: file_io
             * `read_all` con variable local `fp` colisionando con field
             * `this.fp`.
             *
             * Fix: tratar el body como UN SOLO BlockStmt, lo que comparte
             * el @c locals_stack interno entre todos los statements del body.
             */
            std::unique_ptr<ast::Stmt> body_as_stmt(m->body.release());
            rewrite_implicit_this(body_as_stmt, field_names, params_set);
            // Re-inyectar el BlockStmt al field body (el rewrite no lo movio).
            m->body.reset(static_cast<ast::BlockStmt *>(body_as_stmt.release()));
        }
        // Tipo de retorno: VOID para constructores; el declarado para los demas.
        const Type fn_ret = m->is_constructor
                              ? Type{PrimitiveKind::VOID}
                              : type_from_node(m->return_type.get());
        const Type saved_ret = current_fn_return_type_;
        current_fn_return_type_ = fn_ret;
        check_block(m->body.get(), fn_ret);
        current_fn_return_type_ = saved_ret;
        pop_scope();
        current_method_is_static_ = saved_static;
    }

    void TypeChecker::check_block(ast::BlockStmt *b, const Type &fn_return_type) {
        push_scope();
        for (auto &s : b->body) {
            check_stmt(s.get(), fn_return_type);
        }
        pop_scope();
    }

    // F1 NLL - pre-pase: numera stmts en DFS order y para cada IdentExpr
    // registra el stmt_idx de su uso.  Calcula el max per nombre y se lo
    // da al borrow checker.
    void TypeChecker::compute_borrow_last_uses(ast::Stmt *body) {
        if (!body) return;
        std::unordered_map<std::string, uint32_t> last_use;
        uint32_t                                   counter = 0;

        std::function<void(ast::Expr *)> visit_expr;
        std::function<void(ast::Stmt *)> visit_stmt;

        // Recorre la expr y registra el stmt_idx actual (counter) en
        // cualquier IdentExpr.  Para CallExpr, recurse en callee + args.
        // Para los demas, recurse en sub-exprs relevantes.
        visit_expr = [&](ast::Expr *e) {
            if (!e) return;
            switch (e->kind) {
                case ast::NodeKind::IdentExpr: {
                    auto *id = static_cast<ast::IdentExpr *>(e);
                    auto  it = last_use.find(id->name);
                    if (it == last_use.end() || it->second < counter) {
                        last_use[id->name] = counter;
                    }
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
                    for (auto &a : c->args) visit_expr(a.get());
                    return;
                }
                case ast::NodeKind::FieldAccessExpr: {
                    auto *f = static_cast<ast::FieldAccessExpr *>(e);
                    visit_expr(f->base.get());
                    return;
                }
                case ast::NodeKind::IndexExpr: {
                    auto *ix = static_cast<ast::IndexExpr *>(e);
                    visit_expr(ix->base.get());
                    visit_expr(ix->index.get());
                    return;
                }
                case ast::NodeKind::AssignExpr: {
                    auto *as = static_cast<ast::AssignExpr *>(e);
                    visit_expr(as->target.get());
                    visit_expr(as->value.get());
                    return;
                }
                default:
                    return;
            }
        };

        visit_stmt = [&](ast::Stmt *s) {
            if (!s) return;
            // Para BlockStmt no incrementamos: solo es contenedor; check_stmt
            // tampoco lo cuenta como un stmt aparte (se procesa via check_block
            // que itera children).  Mantenemos la simetria con la fase
            // de checkeo.
            if (s->kind == ast::NodeKind::BlockStmt) {
                auto *b = static_cast<ast::BlockStmt *>(s);
                for (auto &sub : b->body) visit_stmt(sub.get());
                return;
            }
            ++counter;
            switch (s->kind) {
                case ast::NodeKind::BlockStmt: {
                    auto *b = static_cast<ast::BlockStmt *>(s);
                    for (auto &sub : b->body) visit_stmt(sub.get());
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<ast::VarDeclStmt *>(s);
                    if (vd->init) visit_expr(vd->init.get());
                    return;
                }
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<ast::ExprStmt *>(s);
                    visit_expr(es->expr.get());
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *is = static_cast<ast::IfStmt *>(s);
                    visit_expr(is->cond.get());
                    if (is->then_branch) visit_stmt(is->then_branch.get());
                    if (is->else_branch) visit_stmt(is->else_branch.get());
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *ws = static_cast<ast::WhileStmt *>(s);
                    visit_expr(ws->cond.get());
                    if (ws->body) visit_stmt(ws->body.get());
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *fs = static_cast<ast::ForStmt *>(s);
                    if (fs->init) visit_stmt(fs->init.get());
                    if (fs->cond) visit_expr(fs->cond.get());
                    if (fs->step) visit_expr(fs->step.get());
                    if (fs->body) visit_stmt(fs->body.get());
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *r = static_cast<ast::ReturnStmt *>(s);
                    if (r->value) visit_expr(r->value.get());
                    return;
                }
                default:
                    return;
            }
        };

        visit_stmt(body);

        // Entregar los last-uses al borrow checker.  El borrow checker
        // los aplica solo a entradas ya registradas en borrows_; las
        // de variables que no son borrows se ignoran silenciosamente.
        for (const auto &kv : last_use) {
            borrow_checker_.set_last_use(kv.first, kv.second);
        }
    }

    void TypeChecker::check_stmt(ast::Stmt *s, const Type &fn_return_type) {
        if (!s) return;
        // F1 NLL: BlockStmt no cuenta como un stmt independiente (delegamos
        // a check_block que itera children).  Para todos los demas, este
        // ES el stmt: incrementar el contador y avanzar el borrow checker
        // (drop NLL de borrows cuyo last_use < current_stmt_idx_).
        if (s->kind != ast::NodeKind::BlockStmt) {
            ++current_stmt_idx_;
            borrow_checker_.advance_stmt(current_stmt_idx_);
        }
        switch (s->kind) {
            case ast::NodeKind::BlockStmt:
                check_block(static_cast<ast::BlockStmt *>(s), fn_return_type);
                return;
            case ast::NodeKind::VarDeclStmt:
                check_var_decl(static_cast<ast::VarDeclStmt *>(s));
                return;
            case ast::NodeKind::ExprStmt: {
                auto *es = static_cast<ast::ExprStmt *>(s);
                if (es->expr) {
                    Type t = check_expr(es->expr.get());
                    // si la expresion es una llamada que retorna
                    // Result<V,E>, el caller DEBE manejar el resultado
                    // (asignar a una var, encadenar con isOk()/value(),
                    // etc).  Descartar un Result en expression-statement
                    // suele ser un bug (errores ignorados silenciosamente).
                    // Mismo principio que Rust con #[must_use].
                    if (t.kind == PrimitiveKind::RESULT
                     && es->expr->kind == ast::NodeKind::CallExpr) {
                        diags_.error(es->loc,
                            "el valor de tipo Result<...> debe ser manejado: "
                            "asigna a una variable y comprueba con isOk()/value()/error(), "
                            "o usa unwrap() para propagar el error");
                    }
                }
                return;
            }
            case ast::NodeKind::IfStmt:
                check_if(static_cast<ast::IfStmt *>(s), fn_return_type);
                return;
            case ast::NodeKind::WhileStmt:
                check_while(static_cast<ast::WhileStmt *>(s), fn_return_type);
                return;
            case ast::NodeKind::ForStmt:
                check_for(static_cast<ast::ForStmt *>(s), fn_return_type);
                return;
            case ast::NodeKind::ForEachStmt: {
                auto *fe = static_cast<ast::ForEachStmt *>(s);
                push_scope();
                // Validar que iter_expr es array.
                Type tcol = fe->iter_expr ? check_expr(fe->iter_expr.get())
                                          : Type{PrimitiveKind::COUNT};
                if (tcol.kind != PrimitiveKind::ARRAY
                 && tcol.kind != PrimitiveKind::COUNT) {
                    diags_.error(fe->loc,
                        "for-each: la coleccion debe ser un array (recibido " +
                        type_to_string(tcol) + ")");
                }
                Type elem_decl = type_from_node(fe->iter_type.get());
                Type elem_actual = (tcol.kind == PrimitiveKind::ARRAY && tcol.pointee)
                                    ? *tcol.pointee
                                    : Type{PrimitiveKind::COUNT};
                if (elem_actual.kind != PrimitiveKind::COUNT
                 && elem_actual != elem_decl) {
                    diags_.error(fe->loc,
                        "for-each: tipo del iterador (" + type_to_string(elem_decl) +
                        ") incompatible con tipo de elemento (" +
                        type_to_string(elem_actual) + ")");
                }
                Symbol sym;
                sym.kind = SymbolKind::Variable;
                sym.type = elem_decl;
                if (!declare(fe->iter_name, sym)) {
                    diags_.error(fe->loc,
                        "for-each: redefinicion de variable: '" + fe->iter_name + "'");
                }
                if (fe->body) check_stmt(fe->body.get(), fn_return_type);
                pop_scope();
                return;
            }
            case ast::NodeKind::ReturnStmt:
                check_return(static_cast<ast::ReturnStmt *>(s), fn_return_type);
                return;
            case ast::NodeKind::BreakStmt:
            case ast::NodeKind::ContinueStmt:
                // En no validamos que esten dentro de un loop;
                // ese check llega en el lowering (donde es trivial).
                return;
            case ast::NodeKind::TryStmt: {
                auto *ts = static_cast<ast::TryStmt *>(s);
                if (ts->body) check_stmt(ts->body.get(), fn_return_type);
                for (auto &cc : ts->catches) {
                    // Validar que el tipo de la excepcion (si se da) es
                    // una clase declarada.
                    if (!cc.exc_class_name.empty()
                     && class_layouts_.find(cc.exc_class_name) == class_layouts_.end()) {
                        diags_.error(cc.loc,
                            "tipo de excepcion no encontrado: '" + cc.exc_class_name + "'");
                    }
                    // Bindear la variable del catch al scope del body.
                    push_scope();
                    if (!cc.var_name.empty() && !cc.exc_class_name.empty()) {
                        Symbol sym;
                        sym.kind = SymbolKind::Variable;
                        sym.type = Type{PrimitiveKind::CLASS};
                        sym.type.struct_name = cc.exc_class_name;
                        if (!declare(cc.var_name, sym)) {
                            diags_.error(cc.loc,
                                "redefinicion de variable en catch: '" + cc.var_name + "'");
                        }
                    }
                    if (cc.body) check_stmt(cc.body.get(), fn_return_type);
                    pop_scope();
                }
                if (ts->finally_body) check_stmt(ts->finally_body.get(), fn_return_type);
                return;
            }
            case ast::NodeKind::ThrowStmt: {
                auto *th = static_cast<ast::ThrowStmt *>(s);
                if (th->value) {
                    Type tv = check_expr(th->value.get());
                    if (tv.kind != PrimitiveKind::CLASS
                     && tv.kind != PrimitiveKind::COUNT) {
                        diags_.error(th->loc,
                            "throw: el valor debe ser una instancia de clase, recibido " +
                            type_to_string(tv));
                    }
                }
                return;
            }
            case ast::NodeKind::SynchronizedStmt: {
                // validar que la expresion-target es CLASS
                // (los monitores solo aplican a objetos GC-managed; primitivos
                // no tienen header de monitor en el ObjectHeader).
                auto *ss = static_cast<ast::SynchronizedStmt *>(s);
                if (ss->target) {
                    Type tv = check_expr(ss->target.get());
                    if (tv.kind != PrimitiveKind::CLASS
                     && tv.kind != PrimitiveKind::COUNT) {
                        diags_.error(ss->loc,
                            "synchronized: el target debe ser una instancia de clase, recibido " +
                            type_to_string(tv));
                    }
                }
                /* incrementa el depth para que wait/notify/notifyAll
                 * dentro del body se acepten.  Decrementa al salir aun si
                 * el body tuvo errores (no bloquea diagnosticos sucesivos). */
                ++synchronized_depth_;
                if (ss->body) check_stmt(ss->body.get(), fn_return_type);
                --synchronized_depth_;
                return;
            }
            case ast::NodeKind::ComptimeBlockStmt: {
                /* bloque comptime { ... } -- scope con vars
                 * mutables + control de flujo completo.  Se procesa con
                 * UN solo pase interleaved: para cada stmt, primero
                 * validamos via check_stmt (annota IdentExprs y registra
                 * vars en comptime_const_locals_), luego ejecutamos via
                 * comptime_eval_stmt (aplica asignaciones, evalua while
                 * con counter mutable, etc).  Esto garantiza que un
                 * static_assert posterior vea el estado actualizado por
                 * las asignaciones anteriores. */
                auto *cb = static_cast<ast::ComptimeBlockStmt *>(s);
                push_comptime_scope();
                ComptimeControl ctrl;
                for (auto &inner : cb->stmts) {
                    if (!inner) continue;
                    /* Validar permisos del stmt.  Otros stmts emiten
                     * error explicito. */
                    bool valid = false;
                    if (inner->kind == ast::NodeKind::VarDeclStmt) {
                        auto *v = static_cast<ast::VarDeclStmt *>(inner.get());
                        if (v->is_comptime) valid = true;
                    } else if (inner->kind == ast::NodeKind::ExprStmt
                            || inner->kind == ast::NodeKind::IfStmt
                            || inner->kind == ast::NodeKind::WhileStmt
                            || inner->kind == ast::NodeKind::DoWhileStmt
                            || inner->kind == ast::NodeKind::ForStmt
                            || inner->kind == ast::NodeKind::BlockStmt
                            || inner->kind == ast::NodeKind::ComptimeBlockStmt
                            || inner->kind == ast::NodeKind::ComptimeForStmt
                            || inner->kind == ast::NodeKind::BreakStmt
                            || inner->kind == ast::NodeKind::ContinueStmt) {
                        valid = true;
                    }
                    if (!valid) {
                        diags_.error(inner->loc,
                            "comptime block: stmt no soportado en contexto comptime");
                        continue;
                    }
                    /* PASS A: annotation pass.  Para VarDeclStmt
                     * comptime, registramos el binding via check_var_decl
                     * (eso lo agrega a comptime_const_locals_).  Para
                     * todo lo demas NO llamamos check_stmt (porque
                     * dispararia static_assert con estado obsoleto u
                     * otras evaluaciones tempranas).  En su lugar
                     * confiamos en que comptime_eval_stmt anota lo que
                     * necesite via check_ident interno (comptime_eval_expr
                     * busca directo en comptime_const_locals_, no necesita
                     * annotation). */
                    if (inner->kind == ast::NodeKind::VarDeclStmt) {
                        auto *v = static_cast<ast::VarDeclStmt *>(inner.get());
                        check_var_decl(v);
                        continue; /* check_var_decl ya evaluo el init */
                    }
                    /* PASS B: execute comptime.  Para ExprStmt con
                     * static_assert (o cualquier CallExpr con efecto
                     * compile-time), llamamos check_expr DENTRO de la
                     * llamada que invoca eval.  Para AssignExpr,
                     * comptime_eval_stmt actualiza el binding.  Para
                     * if/while/for, ejecuta el cuerpo iterativamente. */
                    if (inner->kind == ast::NodeKind::ExprStmt) {
                        auto *es = static_cast<ast::ExprStmt *>(inner.get());
                        if (es->expr) {
                            /* Pre-annotate identifiers via check_expr.
                             * Esto tambien dispara static_assert si lo
                             * hay -- y como se hace AHORA (tras las
                             * asignaciones previas), ve el estado
                             * actualizado. */
                            (void)check_expr(es->expr.get());
                        }
                    }
                    /* Ejecutar el stmt en compile-time. */
                    if (!comptime_eval_stmt(*this, inner.get(), ctrl)) {
                        diags_.error(inner->loc,
                            "comptime block: stmt no evaluable en compile-time");
                        break;
                    }
                    if (ctrl.returned || ctrl.break_seen || ctrl.continue_seen) {
                        ctrl.returned = ctrl.break_seen = ctrl.continue_seen = false;
                        break;
                    }
                }
                pop_comptime_scope();
                return;
            }
            case ast::NodeKind::ComptimeForStmt: {
                /* A.39: comptime for (i in lo..hi) { body } -- evaluamos
                 * lo y hi en compile-time.  El body se chequea UNA vez
                 * con i bindeado al valor de lo (suficiente para validar
                 * tipos en la primera iteracion; el lowering hace el
                 * unroll real clonando el body N veces). */
                auto *cf = static_cast<ast::ComptimeForStmt *>(s);
                if (!cf->lo_expr || !cf->hi_expr) {
                    diags_.error(cf->loc,
                        "comptime for: rango incompleto");
                    return;
                }
                const ComptimeEvalResult lo = comptime_eval_expr(
                    *this, cf->lo_expr.get());
                const ComptimeEvalResult hi = comptime_eval_expr(
                    *this, cf->hi_expr.get());
                if (!lo.ok || !hi.ok || lo.is_str || hi.is_str) {
                    diags_.error(cf->loc,
                        "comptime for: lo y hi deben ser enteros "
                        "comptime-evaluables");
                    return;
                }
                /* Chequear el body con i bindeado a lo (representativo). */
                push_comptime_scope();
                ComptimeConst c;
                c.type  = Type{PrimitiveKind::I64};
                c.value = lo.value;
                register_comptime_local(cf->var_name, std::move(c));
                if (cf->body) check_stmt(cf->body.get(), fn_return_type);
                pop_comptime_scope();
                return;
            }
            default:
                return;
        }
    }

    void TypeChecker::check_var_decl(ast::VarDeclStmt *vd) {
        // BugFix R8 mutation: si el var es `comptime var` (mutable) dentro
        // de un @Macro body, lo convertimos a runtime var (lowering emite
        // ALLOCA + STORE).  El bytecode del macro asi reflejara las
        // mutaciones correctamente.  El AST evaluator mantiene su propio
        // tracking via comptime_const_locals_ + apply_comptime_assign.
        // Para `comptime const` (inmutable) mantenemos el comportamiento
        // original (skip lowering, inline en lower_ident).
        if (current_fn_is_macro_ && vd->is_comptime && !vd->is_const && vd->init) {
            // Evaluar init y registrar para AST eval; pero convertir a
            // runtime var (clear is_comptime).
            const ComptimeEvalResult r =
                comptime_eval_expr(*this, vd->init.get());
            if (r.ok) {
                ComptimeConst c;
                if (vd->type) {
                    c.type = type_from_node(vd->type.get());
                } else if (r.is_str) {
                    c.type = Type{PrimitiveKind::STRING};
                } else {
                    c.type = Type{PrimitiveKind::I64};
                }
                c.is_str     = r.is_str;
                c.is_mutable = true;
                if (r.is_str) c.str_value = r.str;
                else          c.value     = r.value;
                register_comptime_local(vd->name, std::move(c));
            }
            vd->is_comptime = false;  // convertir a runtime
            // FALLTHROUGH: el resto de check_var_decl emite runtime storage.
        }
        // BugFix R8 (read-only): dentro de un @Macro body, las var-decls
        // SIN modificador comptime registran su valor inicial en
        // @c comptime_const_locals_ para que el AST evaluator resuelva
        // IdentExprs cuando otros builtins comptime (comptime_concat,
        // etc.) los necesitan.  NO marcamos is_comptime en el AST.
        if (current_fn_is_macro_ && !vd->is_comptime && vd->init) {
            const ComptimeEvalResult r =
                comptime_eval_expr(*this, vd->init.get());
            if (r.ok) {
                ComptimeConst c;
                if (vd->type) {
                    c.type = type_from_node(vd->type.get());
                } else if (r.is_str) {
                    c.type = Type{PrimitiveKind::STRING};
                } else {
                    c.type = Type{PrimitiveKind::I64};
                }
                c.is_str     = r.is_str;
                c.is_array   = r.is_array;
                c.is_struct  = r.is_struct;
                c.is_mutable = !vd->is_const;
                if (r.is_str)         c.str_value     = r.str;
                else if (r.is_array)  c.array_vals    = r.array_vals;
                else if (r.is_struct) c.struct_fields = r.struct_fields;
                else                  c.value         = r.value;
                register_comptime_local(vd->name, std::move(c));
            }
            // FALLTHROUGH: el resto de check_var_decl emite el var-decl
            // como runtime normal (ALLOCA + STORE).  El AST evaluator usa
            // el comptime local registrado arriba; el VM/bytecode usa la
            // variable runtime con sus mutaciones aplicadas en orden.
        }
        /* `comptime const NAME = expr;` local.  Evalua el init en
         * compile-time y registra en el scope local de comptime const.
         * El lowering lo trata como no-op (no genera ALLOCA ni STORE);
         * cualquier ident posterior queda anotado por check_ident. */
        if (vd->is_comptime) {
            if (!vd->init) {
                diags_.error(vd->loc,
                    std::string("'comptime ") + (vd->is_const ? "const " : "")
                    + vd->name + "' requiere un inicializador");
                return;
            }
            const ComptimeEvalResult r =
                comptime_eval_expr(*this, vd->init.get());
            if (!r.ok) {
                diags_.error(vd->init->loc,
                    std::string("el init de 'comptime ")
                    + (vd->is_const ? "const " : "")
                    + vd->name + "' no es comptime-evaluable");
                return;
            }
            ComptimeConst c;
            /* sugar: si vd->type es nullptr (sugar `comptime X = ...`
             * local sin tipo explicito), inferimos el tipo desde el
             * ComptimeEvalResult.  Misma logica que en el handler global. */
            if (vd->type) {
                c.type = type_from_node(vd->type.get());
            } else if (r.is_str) {
                c.type = Type{PrimitiveKind::STRING};
            } else if (r.is_type) {
                c.type = Type{PrimitiveKind::TYPE_META};
            } else {
                c.type = Type{PrimitiveKind::I64};
            }
            c.is_str     = r.is_str;
            c.is_array   = r.is_array;
            c.is_struct  = r.is_struct;
            c.is_type    = r.is_type;
            c.is_mutable = !vd->is_const;
            if (r.is_str)         c.str_value     = r.str;
            else if (r.is_array)  c.array_vals    = r.array_vals;
            else if (r.is_struct) c.struct_fields = r.struct_fields;
            else if (r.is_type)   c.type_val      = r.type_val;
            else                  c.value         = r.value;
            register_comptime_local(vd->name, std::move(c));
            return;
        }
        Symbol s;
        s.kind     = SymbolKind::Variable;
        /* `auto NAME = init;` o `var NAME = init;` -- inferencia
         * local.  El parser dejo @c vd->type=nullptr y marco infer_type.
         * Computamos el tipo del init aqui y lo aplicamos al binding sin
         * reconstruir el AST.  Falla con error si no hay init. */
        if (vd->infer_type) {
            if (!vd->init) {
                diags_.error(vd->loc,
                    "'auto " + vd->name + "' requiere un inicializador (no se puede inferir sin valor)");
                return;
            }
            s.type = check_expr(vd->init.get());
            if (s.type.kind == PrimitiveKind::VOID) {
                diags_.error(vd->loc,
                    "no se pudo inferir el tipo de '" + vd->name + "' (init devuelve void)");
                return;
            }
        } else {
            s.type = type_from_node(vd->type.get());
        }
        s.is_const = vd->is_const;
        // Captura del alias de reflexion (Class/Method/Field/Object) para
        // habilitar dispatch ergonomico `cls.getMethod(...)` etc.  El TypeNode
        // original era un NamedTypeNode con el nombre del alias; tras
        // type_from_node el tipo subyacente queda como i64.  Si el nombre
        // textual coincide con uno de los aliases magicos, registramos en
        // el Symbol para que `check_field_access` lo recupere despues.
        if (vd->type && vd->type->kind == ast::NodeKind::NamedTypeNode) {
            const auto *nt = static_cast<const ast::NamedTypeNode *>(vd->type.get());
            if (nt->name == "Class"  || nt->name == "Method"
             || nt->name == "Field"  || nt->name == "Object") {
                s.reflection_alias = nt->name;
            }
        }
        // Restricciones para arrays nativos como variables locales:
        //  - El tamano debe ser conocido (T[]) solo se admite como tipo de
        //    parametro de funcion, no como variable.
        //  - El tamano debe ser > 0 (literal entero positivo).
        // El caso T[] como parametro se construye en otro punto (firma de
        // funcion, no aqui), por lo que aqui basta reportar.
        if (s.type.kind == PrimitiveKind::ARRAY && s.type.array_size == 0) {
            // bug4: arrays dinamicos `T[]` con init `new T[N]` o asignacion
            // desde otro array dinamico son legales: el slot guarda el
            // host_ptr al buffer alocado heap.  Solo rechazar si NO hay init
            // (variable sin tamano fijo ni alocacion runtime).
            const bool init_provides_size =
                vd->init
                && (vd->init->kind == ast::NodeKind::NewExpr
                 || vd->init->kind == ast::NodeKind::CallExpr
                 || vd->init->kind == ast::NodeKind::IdentExpr
                 || vd->init->kind == ast::NodeKind::FieldAccessExpr);
            if (!init_provides_size) {
                diags_.error(vd->loc,
                    "el array '" + vd->name +
                    "' requiere un tamano fijo (T[N] con N > 0) o init con `new T[N]`");
            }
        }
        if (!declare(vd->name, s)) {
            diags_.error(vd->loc, "redefinicion de variable: '" + vd->name + "'");
        }
        // Borrow checker: si la variable es @c unique<T>/shared<T>,
        // registrarla como owner (posible objeto de prestamos).  Si la
        // variable es @c borrow<T>/borrow_mut<T>, registrarla como
        // borrower del owner del que provino.  El registro real ocurre
        // tras chequear el init (que es donde sabemos el owner via
        // lend(owner)).
        if (s.type.kind == PrimitiveKind::UNIQUE_PTR
         || s.type.kind == PrimitiveKind::SHARED_PTR) {
            borrow_checker_.declare_owner(vd->name);
        }
        if (vd->init) {
            // si la variable tiene tipo `fn(T1, T2) -> R` y
            // el inicializador es una @c LambdaExpr, propagamos los tipos
            // declarados a los parametros de la lambda que no llevan
            // anotacion explicita (poor-man's bidirectional type checking).
            // Tambien anotamos return_type para que check_lambda lo respete.
            // Esto permite la sintaxis natural sin escribir tipos dos veces:
            //   fn(i32) -> i32 sq = (x) => x * x;       // x deducido a i32
            // Si el numero de parametros no coincide, NO mutamos nada: el
            // check normal generara el diagnostico de aridad.
            if (s.type.kind == PrimitiveKind::FUNCTION
             && vd->init->kind == ast::NodeKind::LambdaExpr) {
                auto *lam = static_cast<ast::LambdaExpr *>(vd->init.get());
                if (lam->params.size() == s.type.fn_params.size()) {
                    for (size_t i = 0; i < lam->params.size(); ++i) {
                        if (!lam->params[i]->type) {
                            // Construir un PrimitiveTypeNode (o NamedTypeNode
                            // para CLASS/STRUCT) que represente el tipo
                            // esperado.  Los tipos primitivos cubren la
                            // mayoria de casos; CLASS/STRUCT se anaden si
                            // es necesario en hitos posteriores.
                            const Type &pt = s.type.fn_params[i];
                            if (pt.kind == PrimitiveKind::CLASS
                             || pt.kind == PrimitiveKind::STRUCT) {
                                auto nt = std::make_unique<ast::NamedTypeNode>();
                                nt->loc  = lam->params[i]->loc;
                                nt->name = pt.struct_name;
                                lam->params[i]->type = std::move(nt);
                            } else {
                                auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                                pn->loc  = lam->params[i]->loc;
                                pn->prim = pt.kind;
                                lam->params[i]->type = std::move(pn);
                            }
                        }
                    }
                    // Tambien propagar return_type si la lambda no lo tenia.
                    if (!lam->return_type && s.type.pointee) {
                        const Type &rt = *s.type.pointee;
                        if (rt.kind == PrimitiveKind::CLASS
                         || rt.kind == PrimitiveKind::STRUCT) {
                            auto nt = std::make_unique<ast::NamedTypeNode>();
                            nt->loc  = lam->loc;
                            nt->name = rt.struct_name;
                            lam->return_type = std::move(nt);
                        } else {
                            auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                            pn->loc  = lam->loc;
                            pn->prim = rt.kind;
                            lam->return_type = std::move(pn);
                        }
                    }
                }
            }
            // Opcion B: auto-envolver init list anonimo en unique_box/shared_box.
            //   unique<Punto> p = {.x=10, .y=20};  ===>
            //   unique<Punto> p = unique_box({.x=10, .y=20});
            // Anotamos target_type_name del init list para que el check
            // del init list valide campos contra el struct destino y
            // devuelva un Type STRUCT (en vez de COUNT).
            if ((s.type.kind == PrimitiveKind::UNIQUE_PTR
              || s.type.kind == PrimitiveKind::SHARED_PTR)
             && s.type.pointee
             && vd->init->kind == ast::NodeKind::InitListExpr
             && s.type.pointee->kind == PrimitiveKind::STRUCT) {
                auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
                il->target_type_name = s.type.pointee->struct_name;
                // Sintetizar CallExpr(unique_box/shared_box, [init_list]).
                auto wrap = std::make_unique<ast::CallExpr>();
                wrap->loc = vd->init->loc;
                auto callee = std::make_unique<ast::IdentExpr>();
                callee->loc  = vd->init->loc;
                callee->name = (s.type.kind == PrimitiveKind::UNIQUE_PTR)
                    ? "unique_box" : "shared_box";
                wrap->callee = std::move(callee);
                wrap->args.push_back(std::move(vd->init));
                vd->init = std::move(wrap);
            }
            // L2.3: si el tipo declarado es un enum generico monomorphizado
            // (e.g. Maybe_i32), push expected_enum_stack para que el RHS
            // `Maybe.Some(42)` o `Maybe.None` resuelva al mangled correcto.
            bool pushed_expected_enum = false;
            if (s.type.kind == PrimitiveKind::STRUCT
             && !s.type.struct_name.empty()) {
                const std::string &mn = s.type.struct_name;
                size_t us = mn.find('_');
                if (us != std::string::npos) {
                    std::string templ = mn.substr(0, us);
                    if (is_generic_enum_template(templ)) {
                        push_expected_enum(templ, mn);
                        pushed_expected_enum = true;
                    }
                }
            }
            // Sprint edge-bugs (2026-06-02): propagar expected_optional_type_
            // y expected_result_type_ al check_expr del init.  Sin esto
            // Optional<Optional<i32>> o = Some(Some(42)) NO infiere el
            // inner Some como Optional<i32> (queda como Optional<i64> por
            // el literal 42).  Mismo patron que check_return ya hacia.
            const Type saved_outer_opt    = expected_optional_type_;
            const Type saved_outer_result = expected_result_type_;
            if (s.type.kind == PrimitiveKind::OPTIONAL) {
                expected_optional_type_ = s.type;
            } else if (s.type.kind == PrimitiveKind::RESULT) {
                expected_result_type_ = s.type;
            }
            Type t = check_expr(vd->init.get());
            expected_optional_type_ = saved_outer_opt;
            expected_result_type_   = saved_outer_result;
            if (pushed_expected_enum) pop_expected_enum();
            // implicit Some: si el tipo declarado es Optional<T> y el
            // init es de tipo T (o asignable a T), envolvemos el init
            // automaticamente con `Some(...)`.  null literal -> None().
            // Esto permite la sintaxis natural:
            //   Optional<i32> a = 50;       -> Some(50)
            //   Optional<i32> a = null;     -> None()
            // sin obligar al usuario a escribir `Some(50)` cada vez.
            if (s.type.kind == PrimitiveKind::OPTIONAL
             && s.type.pointee
             && t.kind != PrimitiveKind::OPTIONAL
             && t.kind != PrimitiveKind::COUNT) {
                if (vd->init->kind == ast::NodeKind::NullLitExpr) {
                    // Reemplazar init con None().
                    auto none_call = std::make_unique<ast::CallExpr>();
                    none_call->loc = vd->init->loc;
                    auto callee = std::make_unique<ast::IdentExpr>();
                    callee->loc  = vd->init->loc;
                    callee->name = "None";
                    none_call->callee = std::move(callee);
                    vd->init = std::move(none_call);
                    t = check_expr(vd->init.get());  // re-tipar
                } else if (types_assignable(*s.type.pointee, t)
                        || class_is_assignable(*s.type.pointee, t)) {
                    // Reemplazar init con Some(init_original).
                    auto some_call = std::make_unique<ast::CallExpr>();
                    some_call->loc = vd->init->loc;
                    auto callee = std::make_unique<ast::IdentExpr>();
                    callee->loc  = vd->init->loc;
                    callee->name = "Some";
                    some_call->callee = std::move(callee);
                    some_call->args.push_back(std::move(vd->init));
                    vd->init = std::move(some_call);
                    t = check_expr(vd->init.get());
                }
            }
            // Coherencia laxa: numericos se promueven en lowering, void*
            // (literal null) es asignable a cualquier T*. si
            // ambos lados son CLASS, permitimos upcast desde clase a
            // interfaz (o supereinterfaz) implementada.  Optional:
            // null es asignable a cualquier referencia (CLASS), modelando
            // semantica nullable por defecto en reference types.
            const bool is_null_lit =
                vd->init->kind == ast::NodeKind::NullLitExpr;
            const bool null_to_class =
                is_null_lit && s.type.kind == PrimitiveKind::CLASS;
            // nonnull: si el tipo declarado lleva el modificador
            // nonnull, rechazar literal null como inicializador.
            if (vd->type && vd->type->is_nonnull && is_null_lit) {
                diags_.error(vd->loc,
                    "no se puede asignar null a una variable 'nonnull' (use !!x para forzar unwrap)");
            }
            if (t.kind != PrimitiveKind::COUNT
             && !types_assignable(s.type, t)
             && !class_is_assignable(s.type, t)
             && !null_to_class) {
                diags_.error(vd->loc,
                    std::string("tipo del inicializador (") + type_to_string(t) +
                    ") incompatible con tipo declarado (" + type_to_string(s.type) + ")");
            }
            // Bug fix 2026-05-23 (LR1): detectar overflow de literales
            // enteros al tipo declarado.  `i32 x = 2147483648;` ahora
            // emite warning.  Cubre IntLitExpr directo y UnaryExpr(Neg,
            // IntLitExpr) para literales negativos.
            bool has_int_lit_init = false;
            int64_t lit_signed = 0;
            uint64_t lit_unsigned = 0;
            if (vd->init->kind == ast::NodeKind::IntLitExpr) {
                has_int_lit_init = true;
                lit_unsigned = static_cast<ast::IntLitExpr *>(vd->init.get())->value;
                lit_signed = (int64_t)lit_unsigned;
            } else if (vd->init->kind == ast::NodeKind::UnaryExpr) {
                auto *u = static_cast<ast::UnaryExpr *>(vd->init.get());
                if (u->op == ast::UnOp::Neg
                 && u->operand
                 && u->operand->kind == ast::NodeKind::IntLitExpr) {
                    has_int_lit_init = true;
                    uint64_t raw = static_cast<ast::IntLitExpr *>(u->operand.get())->value;
                    lit_signed = -(int64_t)raw;
                    lit_unsigned = (uint64_t)lit_signed;
                }
            }
            if (has_int_lit_init && is_integral(s.type.kind)) {
                const uint64_t v = lit_unsigned;
                (void)v;
                const int64_t sv = lit_signed;
                bool overflow = false;
                std::string range_msg;
                switch (s.type.kind) {
                    case PrimitiveKind::I8:
                        if (sv > 127 || sv < -128) overflow = true;
                        range_msg = "i8 [-128, 127]";
                        break;
                    case PrimitiveKind::I16:
                        if (sv > 32767 || sv < -32768) overflow = true;
                        range_msg = "i16 [-32768, 32767]";
                        break;
                    case PrimitiveKind::I32:
                        if (sv > 2147483647LL || sv < -2147483648LL) overflow = true;
                        range_msg = "i32 [-2147483648, 2147483647]";
                        break;
                    case PrimitiveKind::U8:
                        if (sv < 0 || lit_unsigned > 255) overflow = true;
                        range_msg = "u8 [0, 255]";
                        break;
                    case PrimitiveKind::U16:
                        if (sv < 0 || lit_unsigned > 65535) overflow = true;
                        range_msg = "u16 [0, 65535]";
                        break;
                    case PrimitiveKind::U32:
                        if (sv < 0 || lit_unsigned > 4294967295ULL) overflow = true;
                        range_msg = "u32 [0, 4294967295]";
                        break;
                    default: break;
                }
                if (overflow) {
                    diags_.warning(vd->loc,
                        std::string("literal ") + std::to_string(sv) +
                        " fuera del rango del tipo " + type_to_string(s.type) +
                        " (" + range_msg + "); el valor se truncara");
                }
            }
            // Borrow checker: si el var-decl recibio un borrow, asociar
            // el nombre de la variable como borrower del owner correcto.
            // El owner puede venir de tres rutas:
            //   1. lend(owner_var) directo            -> owner = owner_var
            //   2. lend(borrow_var) (reborrow)        -> owner = root via root_owner_of
            //   3. factory(): borrow propagado via F4 -> owner = init->borrow_owner_source
            if ((s.type.kind == PrimitiveKind::BORROW
              || s.type.kind == PrimitiveKind::BORROW_MUT)) {
                std::string owner = vd->init->borrow_owner_source;
                if (owner.empty()
                 && vd->init->kind == ast::NodeKind::CallExpr) {
                    auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
                    if (ce->callee
                     && ce->callee->kind == ast::NodeKind::IdentExpr
                     && ce->args.size() == 1
                     && ce->args[0]->kind == ast::NodeKind::IdentExpr) {
                        auto *cid = static_cast<ast::IdentExpr *>(ce->callee.get());
                        if (cid->name == "lend" || cid->name == "lend_mut") {
                            auto *o = static_cast<ast::IdentExpr *>(ce->args[0].get());
                            owner = borrow_checker_.root_owner_of(o->name);
                            if (owner.empty()) owner = o->name;
                        }
                    }
                }
                if (!owner.empty()) {
                    const bool is_mut = (s.type.kind == PrimitiveKind::BORROW_MUT);
                    borrow_checker_.register_borrow(vd->name, owner, is_mut);
                    // F3 ext - si el init fue un lend()/lend_mut() cuya
                    // fuente era un borrow_mut, marcamos este binding como
                    // reborrow para que su drop restaure el estado
                    // suspendido del owner.
                    if (vd->init->borrow_reborrow_source_is_mut
                     && !vd->init->borrow_reborrow_source_name.empty()) {
                        borrow_checker_.mark_as_reborrow(
                            vd->name,
                            vd->init->borrow_reborrow_source_name);
                    }
                }
            }
        }
    }

    void TypeChecker::check_if(ast::IfStmt *s, const Type &fn_return_type) {
        if (s->cond) {
            Type tc = check_expr(s->cond.get());
            if (tc.kind != PrimitiveKind::BOOL && !is_numeric(tc.kind)) {
                diags_.error(s->cond->loc, "condicion de 'if' debe ser numerica o bool");
            }
        }
        /* `comptime if (cond)` exige que cond sea evaluable
         * 100% en compile-time.  Si no lo es, error claro aqui (no se
         * espera al lowering).  Tambien valida solo la rama elegida --
         * la otra se descarta sin chequear (permite codigo que no compila
         * en la rama no tomada, p.ej. usando features no soportadas para
         * el tipo concreto). */
        if (s->is_comptime) {
            const ComptimeEvalResult r = comptime_eval_expr(*this, s->cond.get());
            if (!r.ok) {
                diags_.error(s->cond->loc,
                    "comptime if: la condicion debe ser evaluable en "
                    "compile-time (literales + builtins comptime + "
                    "operadores logicos/aritmeticos)");
                /* Como fallback, chequear ambas ramas para no perder
                 * diagnosticos posteriores. */
                if (s->then_branch) check_stmt(s->then_branch.get(), fn_return_type);
                if (s->else_branch) check_stmt(s->else_branch.get(), fn_return_type);
                return;
            }
            /* Solo chequeamos la rama tomada.  La otra se descarta. */
            if (r.value != 0) {
                if (s->then_branch) check_stmt(s->then_branch.get(), fn_return_type);
            } else {
                if (s->else_branch) check_stmt(s->else_branch.get(), fn_return_type);
            }
            return;
        }
        if (s->then_branch) check_stmt(s->then_branch.get(), fn_return_type);
        if (s->else_branch) check_stmt(s->else_branch.get(), fn_return_type);
    }

    void TypeChecker::check_while(ast::WhileStmt *s, const Type &fn_return_type) {
        if (s->cond) {
            Type tc = check_expr(s->cond.get());
            if (tc.kind != PrimitiveKind::BOOL && !is_numeric(tc.kind)) {
                diags_.error(s->cond->loc, "condicion de 'while' debe ser numerica o bool");
            }
        }
        if (s->body) check_stmt(s->body.get(), fn_return_type);
    }

    void TypeChecker::check_for(ast::ForStmt *s, const Type &fn_return_type) {
        // Scope adicional para el init del for (estilo C).
        push_scope();
        if (s->init) check_stmt(s->init.get(), fn_return_type);
        if (s->cond) {
            Type tc = check_expr(s->cond.get());
            if (tc.kind != PrimitiveKind::BOOL && !is_numeric(tc.kind)) {
                diags_.error(s->cond->loc, "condicion de 'for' debe ser numerica o bool");
            }
        }
        if (s->step) (void)check_expr(s->step.get());
        if (s->body) check_stmt(s->body.get(), fn_return_type);
        pop_scope();
    }

    void TypeChecker::check_return(ast::ReturnStmt *s, const Type &fn_return_type) {
        if (s->value) {
            // BugFix P1-G1: cuando el caller declara return type FUNCTION
            // y el valor de retorno es una LambdaExpr sin return_type
            // declarado, propagar la firma esperada a la lambda ANTES de
            // check_expr.  Sin esto, check_lambda inferiria VOID como
            // return type y dispararia "return con valor en void" en el
            // body interno.  Mismo patron que check_var_decl ya hace para
            // `fn(...) -> R var = (x) => ...`.
            if (fn_return_type.kind == PrimitiveKind::FUNCTION
             && s->value->kind == ast::NodeKind::LambdaExpr) {
                auto *lam = static_cast<ast::LambdaExpr *>(s->value.get());
                if (lam->params.size() == fn_return_type.fn_params.size()) {
                    for (size_t i = 0; i < lam->params.size(); ++i) {
                        if (!lam->params[i]->type) {
                            const Type &pt = fn_return_type.fn_params[i];
                            if (pt.kind == PrimitiveKind::CLASS
                             || pt.kind == PrimitiveKind::STRUCT) {
                                auto nt = std::make_unique<ast::NamedTypeNode>();
                                nt->loc  = lam->params[i]->loc;
                                nt->name = pt.struct_name;
                                lam->params[i]->type = std::move(nt);
                            } else {
                                auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                                pn->loc  = lam->params[i]->loc;
                                pn->prim = pt.kind;
                                lam->params[i]->type = std::move(pn);
                            }
                        }
                    }
                    if (!lam->return_type && fn_return_type.pointee) {
                        const Type &rt = *fn_return_type.pointee;
                        if (rt.kind == PrimitiveKind::CLASS
                         || rt.kind == PrimitiveKind::STRUCT) {
                            auto nt = std::make_unique<ast::NamedTypeNode>();
                            nt->loc  = lam->loc;
                            nt->name = rt.struct_name;
                            lam->return_type = std::move(nt);
                        } else {
                            auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                            pn->loc  = lam->loc;
                            pn->prim = rt.kind;
                            lam->return_type = std::move(pn);
                        }
                    }
                }
            }
            // Bug fix 2026-05-23: propagar el Result<V,E> / Optional<T>
            // declarado del return type al check_expr antes de bajar Ok/Err/Some.
            // Sin esto, `return Err("lit")` infiere E como i64 (ptr del literal)
            // y rechaza con "Result<i32, i64> incompatible con Result<i32, string>".
            const Type saved_expected_result   = expected_result_type_;
            const Type saved_expected_optional = expected_optional_type_;
            if (fn_return_type.kind == PrimitiveKind::RESULT) {
                expected_result_type_ = fn_return_type;
            } else if (fn_return_type.kind == PrimitiveKind::OPTIONAL) {
                expected_optional_type_ = fn_return_type;
            }
            Type t = check_expr(s->value.get());
            expected_result_type_   = saved_expected_result;
            expected_optional_type_ = saved_expected_optional;
            // Borrow checker R4: si el valor de retorno es un borrow,
            // validar via on_borrow_escape.  El owner_kind del borrow
            // (Local vs Param/Global) decide si el escape es valido.
            // - return borrow de local -> error (lifetime invalido).
            // - return borrow de param -> OK (param vive durante funcion).
            // - return borrow propagado via F4 -> OK si el source es Param.
            if ((t.kind == PrimitiveKind::BORROW
              || t.kind == PrimitiveKind::BORROW_MUT)
              && s->value->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(s->value.get());
                (void)borrow_checker_.on_borrow_escape(
                    id->name, s->loc, "return");
            }
            if (fn_return_type.kind == PrimitiveKind::VOID) {
                diags_.error(s->loc, "'return' con valor en funcion declarada void");
            } else if (t.kind != PrimitiveKind::COUNT && t != fn_return_type) {
                // Aceptar conversiones numericas, asignabilidad de clases
                // (subtypes / interfaces) y compatibilidad de Optional/Result
                // builtins (igual que en check_var_decl).
                const bool numeric_ok = is_numeric(t.kind) && is_numeric(fn_return_type.kind);
                const bool class_ok   = class_is_assignable(fn_return_type, t);
                // null asignable a CLASS / STRING / cualquier referencia.
                // El AST representa `null` como NullLitExpr cuyo tipo es PTR
                // void.  Lo aceptamos en return aunque @c class_is_assignable
                // no lo cubra explicitamente.
                const bool null_ok = (fn_return_type.kind == PrimitiveKind::CLASS
                                      || fn_return_type.kind == PrimitiveKind::STRING
                                      || fn_return_type.kind == PrimitiveKind::PTR)
                                  && t.kind == PrimitiveKind::PTR
                                  && (!t.pointee
                                      || t.pointee->kind == PrimitiveKind::VOID);
                // String literal puro promovible a STRING (mismo patron que
                // `var-decl: string s = "lit"`).  El lowering lo convierte
                // a StringObject via STRMAKE.
                const bool str_lit_ok = fn_return_type.kind == PrimitiveKind::STRING
                                     && t.kind == PrimitiveKind::PTR
                                     && s->value
                                     && s->value->kind == ast::NodeKind::StringLitExpr
                                     && !static_cast<ast::StringLitExpr *>(s->value.get())->is_interpolated();
                if (!numeric_ok && !class_ok && !null_ok && !str_lit_ok) {
                    diags_.error(s->loc,
                        std::string("tipo del valor de retorno (") + type_to_string(t) +
                        ") incompatible con tipo declarado (" + type_to_string(fn_return_type) + ")");
                }
            }
        } else {
            if (fn_return_type.kind != PrimitiveKind::VOID) {
                diags_.error(s->loc, "'return' sin valor en funcion no-void");
            }
        }
    }

    // ---------------------------------------------------------------------
    // Expresiones.
    // ---------------------------------------------------------------------

    Type TypeChecker::check_expr(ast::Expr *e) {
        if (!e) return Type{};
        Type t;
        switch (e->kind) {
            case ast::NodeKind::IntLitExpr:
                // Por defecto los literales enteros son i64.  La promocion
                // / truncacion a la variable destino la decide el lowering.
                t = Type{PrimitiveKind::I64};
                break;
            case ast::NodeKind::FloatLitExpr:
                t = Type{PrimitiveKind::F64};
                break;
            case ast::NodeKind::BoolLitExpr:
                t = Type{PrimitiveKind::BOOL};
                break;
            case ast::NodeKind::CharLitExpr:
                t = Type{PrimitiveKind::CHAR};
                break;
            case ast::NodeKind::StringLitExpr: {
                // En el literal de string se modela como puntero a
                // los bytes en la seccion estatica del modulo (compatible
                // con la convencion FFI de vesta_io: vio_println recibe
                // (proc_ptr, vm_addr, len)).
                //
                // para strings interpolados, validar el tipo de
                // cada expresion ${expr}.  El lowering despachara cada
                // una al builtin nativo apropiado (vio_print_int / _uint
                // / _hex / _float / _bool / _char / _str).
                auto *sl = static_cast<ast::StringLitExpr *>(e);
                if (sl->is_interpolated()) {
                    for (auto &ex : sl->interp_exprs) {
                        Type tx = check_expr(ex.get());
                        ex->result_type = tx;
                        if (tx.kind == PrimitiveKind::VOID
                         || tx.kind == PrimitiveKind::COUNT) {
                            diags_.error(ex->loc,
                                "expresion ${...} no puede ser de tipo void");
                        }
                    }
                }
                t = Type{PrimitiveKind::PTR};
                break;
            }
            case ast::NodeKind::InitListExpr: {
                auto *il = static_cast<ast::InitListExpr *>(e);
                for (auto &el : il->elements) {
                    Type tx = check_expr(el.get());
                    el->result_type = tx;
                }
                // Si el desugar (Opcion B) anoto target_type_name desde el
                // contexto (`unique<Punto> p = {.x=10, .y=20}`), devolvemos
                // el Type STRUCT correspondiente para que unique_box/
                // shared_box vea un tipo concreto en lugar de COUNT.
                if (!il->target_type_name.empty()
                 && struct_layouts_.find(il->target_type_name)
                    != struct_layouts_.end()) {
                    t                = Type{PrimitiveKind::STRUCT};
                    t.struct_name    = il->target_type_name;
                    break;
                }
                // Sin anotacion: tipo dependiente del contexto.  El caller
                // (check_var_decl, lower_var_decl para arrays, etc.) refina.
                t = Type{PrimitiveKind::COUNT};
                break;
            }
            case ast::NodeKind::NullLitExpr:
                // 'null' se modela como void*; el chequeo de asignacion
                // permite asignar void* a cualquier T* sin error.  Se
                // implementa en check_assign / check_var_decl.
                t = Type::make_ptr(Type{PrimitiveKind::VOID});
                break;
            case ast::NodeKind::IdentExpr:
                t = check_ident(static_cast<ast::IdentExpr *>(e));
                break;
            case ast::NodeKind::FieldAccessExpr:
                t = check_field_access(static_cast<ast::FieldAccessExpr *>(e));
                break;
            case ast::NodeKind::BinaryExpr:
                t = check_binary(static_cast<ast::BinaryExpr *>(e));
                break;
            case ast::NodeKind::UnaryExpr:
                t = check_unary(static_cast<ast::UnaryExpr *>(e));
                break;
            case ast::NodeKind::AssignExpr:
                t = check_assign(static_cast<ast::AssignExpr *>(e));
                break;
            case ast::NodeKind::TryExpr: {
                // P2: operador `?` postfix para Result -- early-return.
                // Validar:
                //   1. operand debe ser Result<V, E>
                //   2. la funcion actual debe retornar Result<_, E> con
                //      mismo E (o convertible).
                //   3. result type = V (el payload Ok del operand).
                auto *te = static_cast<ast::TryExpr *>(e);
                Type ot = te->operand ? check_expr(te->operand.get()) : Type{};
                if (ot.kind != PrimitiveKind::RESULT
                 && ot.kind != PrimitiveKind::COUNT) {
                    diags_.error(te->loc,
                        "operador '?' requiere un Result<V,E>, no '"
                        + type_to_string(ot) + "'");
                    t = Type{};
                    break;
                }
                // Verificar que la funcion actual retorne Result<_, E>
                // compatible (mismo E o convertible).
                Type fn_ret = current_fn_return_type_;
                if (fn_ret.kind != PrimitiveKind::RESULT) {
                    diags_.error(te->loc,
                        "operador '?' solo es valido dentro de funciones "
                        "que retornan Result<_, E>; tipo de retorno actual: '"
                        + type_to_string(fn_ret) + "'");
                    t = Type{};
                    break;
                }
                // Comparar tipos E (pointee2 en ambos).
                if (ot.pointee2 && fn_ret.pointee2) {
                    if (!types_assignable(*fn_ret.pointee2, *ot.pointee2)) {
                        diags_.error(te->loc,
                            "operador '?': tipo de error '"
                            + type_to_string(*ot.pointee2)
                            + "' incompatible con el del return type '"
                            + type_to_string(*fn_ret.pointee2) + "'");
                    }
                }
                // Tipo del resultado = V (pointee del operand).
                t = (ot.pointee ? *ot.pointee : Type{PrimitiveKind::I64});
                break;
            }
            case ast::NodeKind::TernaryExpr: {
                /*ternario cond ? then : else.  Tipo resultado =
                 * tipo en comun entre then y else (preferimos el de then;
                 * si else no es asignable a then se reporta error). */
                auto *te = static_cast<ast::TernaryExpr *>(e);
                if (te->cond) {
                    Type ct = check_expr(te->cond.get());
                    if (ct.kind != PrimitiveKind::BOOL && !is_numeric(ct.kind)
                     && ct.kind != PrimitiveKind::COUNT) {
                        diags_.error(te->cond->loc,
                            "condicion ternaria debe ser numerica o bool, no '"
                            + type_to_string(ct) + "'");
                    }
                }
                Type tt = te->then_expr ? check_expr(te->then_expr.get()) : Type{};
                Type et = te->else_expr ? check_expr(te->else_expr.get()) : Type{};
                /* Si los dos son numericos, promovemos al mas ancho.
                 * Si son tipos compatibles, usamos tt como resultado.
                 * Si no, error. */
                if (tt.kind == PrimitiveKind::COUNT) tt = et;
                if (et.kind == PrimitiveKind::COUNT) et = tt;
                if (!types_assignable(tt, et) && !types_assignable(et, tt)) {
                    diags_.error(te->loc,
                        "ternario: ramas con tipos incompatibles '"
                        + type_to_string(tt) + "' y '"
                        + type_to_string(et) + "'");
                }
                t = tt;
                break;
            }
            case ast::NodeKind::CallExpr:
                t = check_call(static_cast<ast::CallExpr *>(e));
                break;
            case ast::NodeKind::IndexExpr:
                t = check_index(static_cast<ast::IndexExpr *>(e));
                break;
            case ast::NodeKind::ThisExpr:
                t = check_this(static_cast<ast::ThisExpr *>(e));
                break;
            case ast::NodeKind::NewExpr:
                t = check_new(static_cast<ast::NewExpr *>(e));
                break;
            case ast::NodeKind::SuperCallExpr: {
                // BugFix R1: super(args) -- valida que estamos en un ctor
                // con super_name no vacio.  No retorna nada util (es como
                // void); validamos args y propagamos.
                auto *sc = static_cast<ast::SuperCallExpr *>(e);
                for (auto &arg : sc->args) check_expr(arg.get());
                if (current_class_.empty()) {
                    diags_.error(sc->loc, "super(...) fuera de cuerpo de clase");
                } else {
                    auto it = class_layouts_.find(current_class_);
                    if (it == class_layouts_.end() || it->second.super_name.empty()) {
                        diags_.error(sc->loc,
                            "super(...) en clase '" + current_class_ +
                            "' que no tiene superclase");
                    }
                }
                t = Type{PrimitiveKind::VOID};
                break;
            }
            case ast::NodeKind::SuperMethodCallExpr: {
                // BugFix R1: super.method(args) -- valida que estamos en
                // metodo de instancia + clase con super_name.  Resuelve
                // el metodo en la jerarquia super y retorna su tipo.
                auto *sm = static_cast<ast::SuperMethodCallExpr *>(e);
                for (auto &arg : sm->args) check_expr(arg.get());
                if (current_class_.empty()) {
                    diags_.error(sm->loc, "super.<metodo>(...) fuera de cuerpo de clase");
                    t = Type{};
                    break;
                }
                auto it = class_layouts_.find(current_class_);
                if (it == class_layouts_.end() || it->second.super_name.empty()) {
                    diags_.error(sm->loc,
                        "super.<metodo>(...) en clase '" + current_class_ +
                        "' que no tiene superclase");
                    t = Type{};
                    break;
                }
                // Buscar el metodo en la jerarquia super (BFS).
                std::string cur = it->second.super_name;
                const ClassMethodInfo *found = nullptr;
                for (int depth = 0; depth < 32; ++depth) {
                    auto it_s = class_layouts_.find(cur);
                    if (it_s == class_layouts_.end()) break;
                    for (const auto &m : it_s->second.methods) {
                        if (!m.is_constructor && m.name == sm->method_name) {
                            found = &m;
                            break;
                        }
                    }
                    if (found) break;
                    if (it_s->second.super_name.empty()) break;
                    cur = it_s->second.super_name;
                }
                if (!found) {
                    diags_.error(sm->loc,
                        "super.<metodo>: '" + sm->method_name +
                        "' no encontrado en jerarquia super de '" + current_class_ + "'");
                    t = Type{};
                } else {
                    t = found->return_type;
                }
                break;
            }
            case ast::NodeKind::SpawnExpr: {
                // spawn { body } - validar el body como un statement
                // ordinario y devolver i64 (PID encoded del proceso hijo).
                auto *se = static_cast<ast::SpawnExpr *>(e);
                // si la policy es Pinned, validar que la
                // expresion del scheduler sea integral (i32/i64/u32/u64).  El
                // lowering hara el modulo num_schedulers en runtime.
                if (se->policy == ast::SpawnExpr::Policy::Pinned && se->sched_idx) {
                    const Type ti = check_expr(se->sched_idx.get());
                    if (ti.kind != PrimitiveKind::I32 && ti.kind != PrimitiveKind::I64
                     && ti.kind != PrimitiveKind::U32 && ti.kind != PrimitiveKind::U64
                     && ti.kind != PrimitiveKind::COUNT) {
                        diags_.error(se->loc,
                            std::string("spawn on(expr): la expresion del scheduler debe ser integral, recibido ")
                            + type_to_string(ti));
                    }
                    se->sched_idx->result_type = ti;
                }
                if (se->body) {
                    // El body se valida en su propio contexto; cualquier
                    // referencia a variables externas se permite (sin closure
                    // lexica en MVP, son globals o errores).  Para evitar
                    // falsos positivos por capturas que el lowering no soporta,
                    // simplemente validamos sintaxis: no bloqueamos returns
                    // ni reglas de funcion.  Falsa simetria con check_function.
                    Type void_t{PrimitiveKind::VOID};
                    push_scope();
                    check_stmt(se->body.get(), void_t);
                    pop_scope();
                }
                t = Type{PrimitiveKind::I64};
                break;
            }
            case ast::NodeKind::LambdaExpr:
                t = check_lambda(static_cast<ast::LambdaExpr *>(e));
                break;
            case ast::NodeKind::MatchExpr:
                t = check_match(static_cast<ast::MatchExpr *>(e));
                break;
            case ast::NodeKind::RSpawnExpr: {
                //  rspawn(node_idx) { body } - spawn distribuido cross-node.
                // Validar:
                //   - node_idx es expresion integral (i32/i64/u32/u64).
                //   - body es block valido; el `return X` se intercepta en
                //     lowering y se transforma en `mov r0, X; hlt` para que
                //     el runtime remoto capture X y lo envie como fulfill.
                // Tipo resultado: i64 (GcHandle del Future).
                auto *re = static_cast<ast::RSpawnExpr *>(e);
                if (re->node_idx) {
                    const Type ti = check_expr(re->node_idx.get());
                    if (ti.kind != PrimitiveKind::I32 && ti.kind != PrimitiveKind::I64
                     && ti.kind != PrimitiveKind::U32 && ti.kind != PrimitiveKind::U64
                     && ti.kind != PrimitiveKind::COUNT) {
                        diags_.error(re->loc,
                            std::string("rspawn(node): la expresion del nodo debe ser integral, recibido ")
                            + type_to_string(ti));
                    }
                    re->node_idx->result_type = ti;
                }
                if (re->body) {
                    Type i64_t{PrimitiveKind::I64};
                    push_scope();
                    check_stmt(re->body.get(), i64_t); // permite return i64
                    pop_scope();
                }
                t = Type{PrimitiveKind::I64};
                break;
            }
            case ast::NodeKind::CastExpr: {
                // Cast C-style `(T) expr`.  Validacion permisiva: el
                // tipo destino se evalua y se chequea el operando, pero
                // no se rechaza ninguna conversion concreta (la decision
                // sobre como bajar la conversion la toma el lowering
                // segun los tipos actual y destino).  Convertir entre
                // punteros (incl. virtual <-> host) es legal; convertir
                // de int a ptr o viceversa tambien.  Quien escriba el
                // cast asume las consecuencias.
                auto *ce = static_cast<ast::CastExpr *>(e);
                if (ce->target_type) {
                    t = type_from_node(ce->target_type.get());
                } else {
                    t = Type{};
                }
                if (ce->operand) {
                    Type to = check_expr(ce->operand.get());
                    ce->operand->result_type = to;
                    if (to.kind == PrimitiveKind::VOID) {
                        diags_.error(ce->loc,
                            "no se puede castear una expresion de tipo void");
                    }
                    // Helper local: validar cast contra el bloque
                    // {explicit from/to T;} + module-privacy.
                    // Devuelve true si la conversion esta declarada y
                    // accesible desde el sitio del cast.
                    auto is_declared_conv =
                        [&](const Type &nt_type, const Type &other,
                            bool from_dir) -> bool {
                        if (nt_type.nominal_id == 0) return false;
                        const auto *info = newtype_info(nt_type.nominal_name);
                        if (!info) return false;
                        const auto &lst = from_dir
                            ? info->from_conversions
                            : info->to_conversions;
                        const bool same_file =
                            (ce->loc.file == info->source_file);
                        for (const auto &ec : lst) {
                            // Match estricto: kind + nominal_id + (struct_name
                            // si aplica).  No usamos types_assignable porque
                            // permite coercion numerica (u32 -> u64), lo que
                            // hace que `explicit from u32` aceptaria u64.
                            // El usuario que declara `from u32` espera
                            // EXACTAMENTE u32, no cualquier integer.
                            if (ec.type.kind != other.kind) continue;
                            if (ec.type.nominal_id != other.nominal_id) continue;
                            if (ec.type.struct_name != other.struct_name) continue;
                            if (ec.is_public || same_file) return true;
                        }
                        return false;
                    };
                    // Newtype @opaque: prohibido cruzar la barrera salvo
                    // que haya una conversion explicita declarada Y la
                    // accesibilidad lo permita (mismo fichero o `public`).
                    // Sin bloque {from/to}: opaque bloquea TODO cast en
                    // cualquier direccion.
                    const bool tgt_opaque = (t.nominal_id != 0 && t.is_opaque);
                    const bool src_opaque = (to.nominal_id != 0 && to.is_opaque);
                    if (tgt_opaque && t.nominal_id != to.nominal_id) {
                        if (!is_declared_conv(t, to, /*from_dir=*/true)) {
                            diags_.error(ce->loc,
                                "no se puede castear a newtype @opaque '"
                                + t.nominal_name + "'; declara "
                                "'explicit from " + type_to_string(to)
                                + ";' en el bloque del typedef"
                                + " (o anyade 'public' si se llama "
                                "desde otro fichero)");
                        }
                    } else if (src_opaque && to.nominal_id != t.nominal_id) {
                        if (!is_declared_conv(to, t, /*from_dir=*/false)) {
                            diags_.error(ce->loc,
                                "no se puede castear desde newtype @opaque '"
                                + to.nominal_name + "'; declara "
                                "'explicit to " + type_to_string(t)
                                + ";' en el bloque del typedef"
                                + " (o anyade 'public' si se llama "
                                "desde otro fichero)");
                        }
                    }
                    // Newtypes NO-opacos con bloque {from/to} declarado:
                    // restringen el cast a las conversiones listadas (mas
                    // estricto que el default no-opaque que permite todo).
                    // Sin bloque: comportamiento default (cast libre con
                    // el underlying).
                    if (!tgt_opaque && t.nominal_id != 0
                     && t.nominal_id != to.nominal_id) {
                        const auto *info = newtype_info(t.nominal_name);
                        if (info && !info->from_conversions.empty()
                         && !is_declared_conv(t, to, /*from_dir=*/true)) {
                            diags_.error(ce->loc,
                                "cast a newtype '" + t.nominal_name
                                + "': '" + type_to_string(to)
                                + "' no esta en su bloque "
                                "{explicit from ...;}");
                        }
                    }
                    if (!src_opaque && to.nominal_id != 0
                     && to.nominal_id != t.nominal_id) {
                        const auto *info = newtype_info(to.nominal_name);
                        if (info && !info->to_conversions.empty()
                         && !is_declared_conv(to, t, /*from_dir=*/false)) {
                            diags_.error(ce->loc,
                                "cast desde newtype '" + to.nominal_name
                                + "': '" + type_to_string(t)
                                + "' no esta en su bloque "
                                "{explicit to ...;}");
                        }
                    }
                }
                break;
            }
            default:
                t = Type{};
                break;
        }
        e->result_type = t;
        return t;
    }

    Type TypeChecker::check_this(ast::ThisExpr *e) {
        if (current_class_.empty()) {
            diags_.error(e->loc,
                "'this' solo es valido dentro del cuerpo de un metodo de instancia");
            return Type{};
        }
        if (current_method_is_static_) {
            diags_.error(e->loc,
                "'this' no es accesible dentro de un metodo 'static'");
            return Type{};
        }
        return Type{PrimitiveKind::CLASS, current_class_};
    }

    Type TypeChecker::check_new(ast::NewExpr *e) {
        // Phase M.7.c: namespace qualified `new ui.Button(...)`.
        // Si class_name contiene `.`, lo traducimos al mangled label
        // ANTES de que el resto del check_new procese.  Asi el resto
        // del codigo (lookup en class_layouts_, llamada al ctor, etc.)
        // ve el nombre interno (`ui__Button`) sin necesidad de cambios.
        {
            size_t dot = e->class_name.find('.');
            if (dot != std::string::npos) {
                const std::string ns_name = e->class_name.substr(0, dot);
                const std::string sym_name = e->class_name.substr(dot + 1);
                const Symbol *ns_sym = lookup(ns_name);
                if (ns_sym && ns_sym->kind == SymbolKind::Namespace
                 && ns_sym->ns_index < imported_namespaces_.size()) {
                    const auto &ns = imported_namespaces_[ns_sym->ns_index];
                    auto its = ns.by_name.find(sym_name);
                    if (its != ns.by_name.end()) {
                        const auto &sym = ns.symbols[its->second];
                        e->class_name = sym.mangled_label;
                    }
                }
            }
        }
        // bug4: array allocation `new T[N]`.  Validar count (debe ser int),
        // resolver el elem type, devolver T[] (array de host_ptr).
        if (e->array_size) {
            Type count_t = check_expr(e->array_size.get());
            if (count_t.kind != PrimitiveKind::I8 && count_t.kind != PrimitiveKind::I16
             && count_t.kind != PrimitiveKind::I32 && count_t.kind != PrimitiveKind::I64
             && count_t.kind != PrimitiveKind::U8 && count_t.kind != PrimitiveKind::U16
             && count_t.kind != PrimitiveKind::U32 && count_t.kind != PrimitiveKind::U64
             && count_t.kind != PrimitiveKind::COUNT) {
                diags_.error(e->array_size->loc,
                    "new T[N]: el tamano del array debe ser un tipo entero, recibido '"
                    + type_to_string(count_t) + "'");
            }
            // Resolver elem_type desde class_name.  Puede ser:
            //   - Primitivo (i32, f64, string, etc.) -> PrimitiveKind correspondiente.
            //   - Clase user (PrimitiveKind::CLASS).
            //   - Struct value-type (PrimitiveKind::STRUCT).
            //   - Enum (PrimitiveKind::STRUCT con enum_layouts_ entry).
            Type elem_t;
            auto pk_from_name = [](const std::string &n) -> PrimitiveKind {
                if (n == "i8") return PrimitiveKind::I8;
                if (n == "i16") return PrimitiveKind::I16;
                if (n == "i32") return PrimitiveKind::I32;
                if (n == "i64") return PrimitiveKind::I64;
                if (n == "u8" || n == "char") return PrimitiveKind::U8;
                if (n == "u16") return PrimitiveKind::U16;
                if (n == "u32") return PrimitiveKind::U32;
                if (n == "u64") return PrimitiveKind::U64;
                if (n == "f32" || n == "float")  return PrimitiveKind::F32;
                if (n == "f64" || n == "double") return PrimitiveKind::F64;
                if (n == "bool") return PrimitiveKind::BOOL;
                if (n == "string") return PrimitiveKind::STRING;
                return PrimitiveKind::COUNT;
            };
            PrimitiveKind pk = pk_from_name(e->class_name);
            // BugFix R5: builtins genericos Optional/Result/Future como
            // tipo elemento del array.
            bool elem_set = false;
            if (pk == PrimitiveKind::COUNT
             && (e->class_name == "Optional" || e->class_name == "Result"
              || e->class_name == "Future")) {
                if (e->type_args.empty()) {
                    diags_.error(e->loc,
                        "new " + e->class_name + "<T>[N]: falta argumento de tipo <T>");
                    e->result_type = Type{};
                    return Type{};
                }
                Type inner = type_from_node(e->type_args[0].get());
                if (e->class_name == "Optional") {
                    elem_t = Type::make_optional(std::move(inner));
                } else if (e->class_name == "Future") {
                    elem_t = Type::make_future(std::move(inner));
                } else {
                    if (e->type_args.size() < 2) {
                        diags_.error(e->loc,
                            "new Result<V,E>[N]: faltan 2 argumentos de tipo");
                        e->result_type = Type{};
                        return Type{};
                    }
                    Type err_t = type_from_node(e->type_args[1].get());
                    elem_t = Type::make_result(std::move(inner), std::move(err_t));
                }
                e->type_args.clear();
                elem_set = true;
            }
            if (!elem_set) {
                if (pk != PrimitiveKind::COUNT) {
                    elem_t = Type{pk};
                } else if (class_layouts_.find(e->class_name) != class_layouts_.end()) {
                    elem_t = Type{PrimitiveKind::CLASS};
                    elem_t.struct_name = e->class_name;
                } else if (struct_layouts_.find(e->class_name) != struct_layouts_.end()) {
                    elem_t = Type{PrimitiveKind::STRUCT};
                    elem_t.struct_name = e->class_name;
                } else if (enum_layouts_.find(e->class_name) != enum_layouts_.end()) {
                    elem_t = Type{PrimitiveKind::STRUCT};
                    elem_t.struct_name = e->class_name;
                } else {
                    diags_.error(e->loc, "new T[N]: tipo desconocido '" + e->class_name + "'");
                    e->result_type = Type{};
                    return Type{};
                }
            }
            // Tipo resultado: ARRAY de elem_t (host, is_virtual=false).
            // size=0 = decay-to-pointer (dynamic size, no conocido en
            // compile-time).
            Type rt = Type::make_array(elem_t, /*size=*/0, /*virt=*/false);
            e->result_type = rt;
            return rt;
        }
        // Validar argumentos primero (siempre se chequean para reportar
        // errores en sus subexpresiones aunque la clase sea desconocida).
        std::vector<Type> arg_types;
        arg_types.reserve(e->args.size());
        for (auto &a : e->args) arg_types.push_back(check_expr(a.get()));

        // generics: si NewExpr trae type_args, redirigimos al nombre
        // mangled de la clase monomorphizada (la cual fue generada en el
        // pre-pase de run()).
        // Bug fix 2026-05-23: el flag is_mangled previene re-mutacion del
        // class_name cuando check_new se invoca multiples veces sobre el
        // mismo NewExpr (e.g. por compound assign que re-evalua RHS, o por
        // reuso del AST entre pases).  Sin esto, `Node` -> `Node_i32` en
        // la primera llamada y `Node_i32` -> `Node_i32_i32` en la segunda.
        if (!e->type_args.empty() && !e->is_mangled) {
            std::vector<Type> targs;
            targs.reserve(e->type_args.size());
            for (auto &ta : e->type_args) targs.push_back(type_from_node(ta.get()));
            e->class_name = e->class_name + "_" + mangle_args(targs);
            e->is_mangled = true;
        }

        auto it = class_layouts_.find(e->class_name);
        if (it == class_layouts_.end()) {
            diags_.error(e->loc, "clase desconocida: '" + e->class_name + "'");
            return Type{};
        }
        const ClassLayout &cls = it->second;

        // BugFix R4: clases excepcion estandar (is_runtime_predefined=true)
        // no tienen constructor Vex; aceptan 1 arg string (message).
        // Devolvemos directamente CLASS sin validar ctor.  El lowering
        // detecta el caso y emite newobj + store message inline.
        if (cls.is_runtime_predefined && cls.name != "FatalError") {
            if (arg_types.size() != 1) {
                diags_.error(e->loc,
                    "constructor de '" + e->class_name +
                    "' espera 1 argumento (message)");
            } else {
                const Type &ta = arg_types[0];
                if (ta.kind != PrimitiveKind::STRING
                 && ta.kind != PrimitiveKind::PTR
                 && ta.kind != PrimitiveKind::COUNT) {
                    diags_.error(e->args[0]->loc,
                        "constructor de '" + e->class_name +
                        "': message debe ser string");
                }
            }
            return Type{PrimitiveKind::CLASS, e->class_name};
        }

        // Localizar el constructor: debe tener el mismo nombre que la
        // clase y is_constructor=true.  Si hay varios, elegimos el que
        // encaje con la lista de argumentos por aridad estricta (overload
        // resolution mejorada llegara).
        const ClassMethodInfo *ctor = nullptr;
        for (const auto &m : cls.methods) {
            if (!m.is_constructor) continue;
            if (m.param_types.size() != arg_types.size()) continue;
            ctor = &m;
            break;
        }
        if (!ctor) {
            // Si la clase no declara ningun constructor explicito,
            // permitimos new X() sin args (constructor implicito).
            const bool has_any_ctor = std::any_of(
                cls.methods.begin(), cls.methods.end(),
                [](const ClassMethodInfo &m) { return m.is_constructor; });
            if (has_any_ctor || !e->args.empty()) {
                diags_.error(e->loc,
                    "no existe constructor de '" + e->class_name +
                    "' con " + std::to_string(arg_types.size()) + " argumentos");
            }
        } else {
            // Verificar tipos de cada argumento contra el constructor.
            for (size_t i = 0; i < arg_types.size(); ++i) {
                const Type &ta = arg_types[i];
                const Type &tp = ctor->param_types[i];
                if (ta.kind == PrimitiveKind::COUNT) continue;
                if (!types_assignable(tp, ta)) {
                    diags_.error(e->args[i]->loc,
                        std::string("argumento ") + std::to_string(i + 1) +
                        " del constructor '" + e->class_name + "': tipo (" +
                        type_to_string(ta) + ") incompatible con parametro (" +
                        type_to_string(tp) + ")");
                }
            }
        }
        return Type{PrimitiveKind::CLASS, e->class_name};
    }

    Type TypeChecker::check_index(ast::IndexExpr *e) {
        // p[i] requiere base PTR o ARRAY y index entero.  El resultado es
        // el tipo del elemento.  Subscript sobre cualquier otro tipo es
        // un error claro.
        if (!e->base) {
            diags_.error(e->loc, "subscript sin base");
            return Type{};
        }
        const Type bt = check_expr(e->base.get());
        if (e->index) (void)check_expr(e->index.get());
        const bool is_ptr_like =
            (bt.kind == PrimitiveKind::PTR || bt.kind == PrimitiveKind::ARRAY)
         && static_cast<bool>(bt.pointee);
        if (!is_ptr_like) {
            diags_.error(e->loc,
                std::string("'[]' requiere un puntero o array, recibido ")
                + type_to_string(bt));
            return Type{};
        }
        if (bt.pointee->kind == PrimitiveKind::VOID) {
            diags_.error(e->loc, "'[]' no puede indexar void*");
            return Type{};
        }
        if (e->index) {
            const Type it = e->index->result_type;
            if (!is_integral(it.kind)) {
                diags_.error(e->loc,
                    std::string("indice de '[]' debe ser entero, recibido ")
                    + type_to_string(it));
            }
        }
        return *bt.pointee;
    }

    // ---------------------------------------------------------------------
    // closures: check_lambda.
    //
    // Diseno:
    //   - Construye Type{FUNCTION, params, return_type} a partir de la
    //     firma sintactica de la lambda.  Los parametros sin tipo
    //     declarado quedan como VOID temporalmente; el contexto (assign /
    //     var-decl con tipo fn(...)) puede resolverlos a posteriori en
    //     una fase futura (MVP: exigimos tipos explicitos en parametros
    //     o defaults a i64).
    //   - El analisis de capturas usa @c lambda_stack_: empuja el ctx
    //     antes de chequear el body y check_ident detecta las referencias
    //     externas para anyadirlas a expr->captures sin duplicados.
    //   - El return_type se infiere del tipo del primer ReturnStmt.  Para
    //     simplificar la implementacion, MVP: si el body termina en
    //     `return X;` con X de tipo T, usar T.  Si no hay return, asumir
    //     VOID.  Validacion de coherencia entre multiples returns queda
    //     para una fase posterior.
    // ---------------------------------------------------------------------
    Type TypeChecker::check_lambda(ast::LambdaExpr *e) {
        // Construir lista de tipos de parametros.  Los parametros sin
        // tipo declarado se asumen i64 (el tipo "todo cabe" de Vex).
        // Inferencia desde contexto (asignacion a fn(T1, T2) -> R) queda
        // como mejora futura: aqui solo soportamos tipos explicitos o
        // i64 por defecto.
        std::vector<Type> param_types;
        param_types.reserve(e->params.size());
        for (auto &p : e->params) {
            Type pt;
            if (p->type) {
                pt = type_from_node(p->type.get());
            } else {
                // Default i64 cuando no hay anotacion.  Suficiente para
                // MVP (la mayoria de lambdas cortas usan enteros); el
                // usuario puede anotar el tipo si quiere otro.
                pt = Type{PrimitiveKind::I64};
            }
            param_types.push_back(pt);
        }

        // Tipo de retorno declarado, o VOID provisional para inferir del body.
        Type return_t;
        bool return_t_declared = false;
        if (e->return_type) {
            return_t = type_from_node(e->return_type.get());
            return_t_declared = true;
        } else {
            return_t = Type{PrimitiveKind::VOID};
        }

        // Empujar el contexto de lambda ANTES de abrir el scope local: el
        // outer_depth debe reflejar el numero de scopes existentes en el
        // momento previo a entrar a la lambda, no incluyendo el scope de
        // sus propios parametros.
        const size_t outer_depth = scopes_.size();
        lambda_stack_.push_back(LambdaCtx{e, outer_depth});

        // Scope local para los parametros.
        push_scope();
        for (size_t i = 0; i < e->params.size(); ++i) {
            Symbol sym;
            sym.kind = SymbolKind::Param;
            sym.type = param_types[i];
            (void)declare(e->params[i]->name, std::move(sym));
        }

        // Type-check del body.  Usamos return_t como expected_return en
        // check_stmt; cuando no hay anotacion explicita, pasamos VOID y
        // luego inferimos del primer return encontrado.
        if (e->body) {
            check_stmt(e->body.get(), return_t);
            // Si el usuario no declaro return_type explicito, inferimos
            // del primer ReturnStmt encontrado en el body.  El recorrido
            // ya fue hecho por check_stmt; aqui solo extraemos el tipo
            // del primer return statement directo en el body.
            if (!return_t_declared) {
                for (auto &st : e->body->body) {
                    if (st && st->kind == ast::NodeKind::ReturnStmt) {
                        auto *rs = static_cast<ast::ReturnStmt *>(st.get());
                        if (rs->value && rs->value->result_type.kind != PrimitiveKind::VOID) {
                            return_t = rs->value->result_type;
                        }
                        break; // primer return manda
                    }
                }
            }
        }

        pop_scope();
        lambda_stack_.pop_back();

        return Type::make_function(std::move(param_types), std::move(return_t));
    }

    // ---------------------------------------------------------------------
    // ADTs: check_match.
    //
    // Validaciones:
    //   1. Scrutinee debe ser de tipo enum (Type{STRUCT, name} con
    //      enum_layouts_[name] presente).
    //   2. Cada arm debe nombrar una variante existente (o '_').
    //   3. bindings.size() == variant.field_types.size().
    //   4. Bindings se introducen como variables locales tipadas en el
    //      scope del body (push/pop scope por arm).
    //   5. Exhaustividad: error si ninguna arm es '_' y faltan
    //      variantes por cubrir.
    // ---------------------------------------------------------------------
    Type TypeChecker::check_match(ast::MatchExpr *e) {
        if (!e->scrutinee) {
            diags_.error(e->loc, "match: scrutinee nulo");
            return Type{};
        }
        const Type st = check_expr(e->scrutinee.get());
        e->scrutinee->result_type = st;
        if (st.kind != PrimitiveKind::STRUCT) {
            diags_.error(e->scrutinee->loc,
                std::string("match: el scrutinee debe ser un valor de tipo enum, recibido ") +
                type_to_string(st));
            return Type{};
        }
        auto it = enum_layouts_.find(st.struct_name);
        if (it == enum_layouts_.end()) {
            diags_.error(e->scrutinee->loc,
                std::string("match: '") + st.struct_name +
                "' no es un enum (es struct?)");
            return Type{};
        }
        const EnumLayout &elay = it->second;

        bool has_default = false;
        std::unordered_map<std::string, bool> covered;
        for (auto &arm : e->arms) {
            if (arm.variant_name == "_") {
                has_default = true;
                // Default arm: no bindings, body en scope vacio adicional.
                // Usamos current_fn_return_type_ para que returns dentro
                // del body validen con el tipo correcto de la funcion
                // enclosing.
                push_scope();
                if (arm.body) {
                    check_stmt(arm.body.get(), current_fn_return_type_);
                }
                pop_scope();
                continue;
            }
            // Buscar la variante por nombre.
            const EnumVariantInfo *var = nullptr;
            for (const auto &v : elay.variants) {
                if (v.name == arm.variant_name) { var = &v; break; }
            }
            if (!var) {
                diags_.error(arm.loc,
                    std::string("variante desconocida '") + arm.variant_name +
                    "' en enum '" + elay.name + "'");
                continue;
            }
            // Bug fix 2026-05-23: solo marcamos como cubierta totalmente
            // si NO tiene guard.  Arms con guard NO cuentan para
            // exhaustividad (el guard puede ser falso en runtime).
            if (!arm.guard) covered[var->name] = true;
            // Validar aridad de bindings.
            if (arm.bindings.size() != var->field_types.size()) {
                diags_.error(arm.loc,
                    std::string("variante '") + var->name +
                    "': esperados " + std::to_string(var->field_types.size()) +
                    " bindings, recibidos " + std::to_string(arm.bindings.size()));
            }
            // Push scope, bind cada binding con su tipo, lower body.
            push_scope();
            const size_t n = std::min(arm.bindings.size(), var->field_types.size());
            for (size_t i = 0; i < n; ++i) {
                Symbol sym;
                sym.kind = SymbolKind::Variable;
                sym.type = var->field_types[i];
                if (!declare(arm.bindings[i], std::move(sym))) {
                    diags_.error(arm.loc,
                        "binding duplicado en patron: '" + arm.bindings[i] + "'");
                }
            }
            // Bug fix 2026-05-23: validar tipo del guard.
            if (arm.guard) {
                Type tg = check_expr(arm.guard.get());
                if (tg.kind != PrimitiveKind::BOOL
                 && !is_numeric(tg.kind)
                 && tg.kind != PrimitiveKind::COUNT) {
                    diags_.error(arm.loc,
                        "el guard del case debe ser una expresion booleana, no '" +
                        type_to_string(tg) + "'");
                }
            }
            if (arm.body) {
                check_stmt(arm.body.get(), current_fn_return_type_);
            }
            pop_scope();
        }

        // Exhaustividad: si no hay default, todas las variantes deben
        // estar cubiertas (al menos una arm cada una).
        if (!has_default) {
            std::vector<std::string> missing;
            for (const auto &v : elay.variants) {
                if (!covered.count(v.name)) missing.push_back(v.name);
            }
            if (!missing.empty()) {
                std::string msg = "match no exhaustivo: faltan variantes:";
                for (const auto &m : missing) msg += " " + m;
                msg += " (anyade una arm por cada una o usa 'case _ =>' como default)";
                diags_.error(e->loc, msg);
            }
        }

        // En MVP el match es statement-like, no produce valor utilizable
        // como expresion (cada arm puede tener su propio efecto).
        return Type{PrimitiveKind::VOID};
    }

    Type TypeChecker::check_ident(ast::IdentExpr *e) {
        /* A.39: si el ident resuelve a un comptime const (local o
         * global), anotamos el valor en el AST.  El lowering lee la
         * marca y emite CONST directo sin volver a consultar la tabla
         * (que puede haber sido pop()-ada por scope locals).  Esto
         * preserva los comptime const locales a traves del boundary
         * type-check -> lowering. */
        {
            /* Buscar en stack de scopes locales primero. */
            for (auto sc = comptime_const_locals_.rbegin();
                 sc != comptime_const_locals_.rend(); ++sc) {
                auto it = sc->find(e->name);
                if (it != sc->end()) {
                    // BugFix R8 mutation: en @Macro body, NO anotar
                    // comptime_const_resolved si el var es mutable (puede
                    // tener escrituras runtime que el lowering debe leer
                    // del stack slot).  El AST evaluator sigue accediendo
                    // a comptime_const_locals_ directamente.  Solo para
                    // vars con tipo declarado (not auto-register).
                    if (current_fn_is_macro_ && it->second.is_mutable) {
                        e->result_type = it->second.type;
                        return e->result_type;
                    }
                    e->comptime_const_resolved = true;
                    if (it->second.is_str) {
                        e->comptime_const_is_str = true;
                        e->comptime_const_str    = it->second.str_value;
                    } else {
                        e->comptime_const_int = it->second.value;
                    }
                    e->result_type = it->second.type;
                    return e->result_type;
                }
            }
            /* Tabla global. */
            auto it = comptime_const_values_.find(e->name);
            if (it != comptime_const_values_.end()) {
                e->comptime_const_resolved = true;
                if (it->second.is_str) {
                    e->comptime_const_is_str = true;
                    e->comptime_const_str    = it->second.str_value;
                } else {
                    e->comptime_const_int = it->second.value;
                }
                e->result_type = it->second.type;
                return e->result_type;
            }
        }
        size_t depth = 0;
        const Symbol *s = lookup_with_depth(e->name, &depth);
        if (!s) {
            // Aliases magicos para reflexion estatica:
            // `Class`, `Method`, `Field`, `Object` pueden aparecer como
            // base de una llamada estatica `Class.forName(...)` SIN haber
            // sido declarados como variable.  En ese contexto NO son
            // identificadores resolubles; el dispatch los reconoce en
            // `check_call` con la forma estatica.  Aqui devolvemos un
            // tipo i64 silencioso para que `check_expr` no reporte error.
            // Si el ident "Class" aparece fuera de ese contexto (e.g.
            // como expresion suelta `i32 x = Class;`), el lowering no
            // sabra que hacer y eso si fallara.  En la practica el unico
            // uso valido es como base de un FieldAccessExpr.
            if (e->name == "Class"  || e->name == "Method"
             || e->name == "Field"  || e->name == "Object") {
                Type t{PrimitiveKind::I64};
                e->result_type = t;
                return t;
            }
            diags_.error(e->loc, "nombre no declarado: '" + e->name + "'");
            return Type{};
        }
        // closures: si estamos dentro del body de una lambda y el
        // identificador resuelve a un scope que existia ANTES de entrar a
        // la lambda, hay que capturarlo en el env block.  Procesamos el
        // stack de lambdas desde el TOPE (la mas interna) hacia abajo:
        // cada lambda cuyo outer_depth excede el depth del lookup necesita
        // capturar el nombre.  Captures transitivas: si la lambda interna
        // captura un nombre que no esta en su outer scope pero si en uno
        // mas externo, registramos en TODAS las lambdas intermedias para
        // que el lowering construya la cadena correcta de envs.
        //
        // Solo capturamos variables (Variable / Param), no funciones top-
        // level ni clases ni metodos.  Una funcion global sigue siendo
        // accesible por nombre desde cualquier lambda sin pasar por env.
        if (s->kind != SymbolKind::Function && !lambda_stack_.empty()) {
            for (auto &ctx : lambda_stack_) {
                if (depth < ctx.outer_depth) {
                    // No duplicar: si ya esta registrada como captura, saltar.
                    bool already = false;
                    for (auto &nm : ctx.expr->captures) {
                        if (nm == e->name) { already = true; break; }
                    }
                    if (!already) {
                        ctx.expr->captures.push_back(e->name);
                        ctx.expr->capture_types.push_back(s->type);
                    }
                }
            }
        }
        if (s->kind == SymbolKind::Function) {
            // Tratamos las funciones como un tipo VOID a efectos de
            // inferencia cuando aparecen sin call directo; el caso CALL
            // (CallExpr) lo manejara especificamente.  Pasar una funcion
            // libre como argumento a un parametro @c fn(T) -> R esta
            // soportado y se promociona explicitamente a function value
            // en @c check_call (cubre el caso first-class).
            return Type{};
        }
        return s->type;
    }

    Type TypeChecker::check_field_access(ast::FieldAccessExpr *e) {
        // ADTs: detectar variante sin payload `Color.Red` (sin
        // parens).  Si la base es un identificador que nombra un enum
        // y el field_name es una variante de aridad 0, lo tratamos como
        // CONSTRUCTOR sin argumentos y devolvemos Type{STRUCT, enum_name}.
        // Si la variante tiene payload no-vacio, reportamos error
        // sugiriendo invocar con argumentos.
        if (e->base
         && e->base->kind == ast::NodeKind::IdentExpr) {
            auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
            // Phase M.7: namespace qualified access (`lib_a.valor_a`).
            // Si el IdentExpr base resuelve a un Symbol::Namespace, el
            // field_name es un simbolo del namespace; devolvemos su tipo
            // (return type para FUNCTION, var_type para Variable).
            // Marcamos property_kind=4 para que el lowering reconozca
            // que debe emitir CALL al mangled_label.
            {
                const Symbol *ns_sym = lookup(base_id->name);
                if (ns_sym && ns_sym->kind == SymbolKind::Namespace) {
                    // L.26: marcar el namespace como referenciado para
                    // que el linter de "import no se usa" no genere
                    // falsos positivos.  El `lookup` plano no toca
                    // @c referenced_names_ ; aqui sabemos que el acceso
                    // namespace.X tuvo exito, asi que marcamos manualmente.
                    referenced_names_.insert(base_id->name);
                    if (ns_sym->ns_index < imported_namespaces_.size()) {
                        const auto &ns = imported_namespaces_[ns_sym->ns_index];
                        auto its = ns.by_name.find(e->field_name);
                        if (its == ns.by_name.end()) {
                            diags_.error(e->loc,
                                "el namespace '" + base_id->name +
                                "' no tiene un simbolo llamado '" +
                                e->field_name + "' (modulo '" +
                                ns.module_name + "')");
                            return Type{};
                        }
                        const auto &sym = ns.symbols[its->second];
                        e->property_kind = 4;                 // namespace member
                        e->ns_index      = ns_sym->ns_index;  // M.7: para lowering
                        // Para functions, el "tipo" del FieldAccess es VOID
                        // (no es una expresion valor); el call site lo trata
                        // como una callable.  Pero retornamos un Type
                        // FUNCTION para que `check_call` lo detecte.
                        if (sym.kind == 0) {
                            // function
                            Type t = Type::make_function(sym.sig.param_types,
                                                          sym.sig.return_type);
                            e->result_type = t;
                            return t;
                        }
                        if (sym.kind == 2) {
                            // TypeAlias (struct/class/enum/typedef cross-module).
                            // Devolver un Type que apunte al layout mangled
                            // para que el outer FieldAccess (`ns.Type.Variant`)
                            // o constructor (`new ns.Class(...)`) pueda
                            // resolver el layout correcto en class_layouts_/
                            // struct_layouts_/enum_layouts_.
                            if (enum_layouts_.find(sym.mangled_label) != enum_layouts_.end()) {
                                // Enum types ref: convencion existente usa
                                // PrimitiveKind::STRUCT + struct_name (el name
                                // coincide con un layout en enum_layouts_).
                                Type t{PrimitiveKind::STRUCT, sym.mangled_label};
                                e->result_type = t;
                                return t;
                            }
                            if (class_layouts_.find(sym.mangled_label) != class_layouts_.end()) {
                                Type t{PrimitiveKind::CLASS, sym.mangled_label};
                                e->result_type = t;
                                return t;
                            }
                            if (struct_layouts_.find(sym.mangled_label) != struct_layouts_.end()) {
                                Type t{PrimitiveKind::STRUCT, sym.mangled_label};
                                e->result_type = t;
                                return t;
                            }
                            // typedef alias: resolver al subyacente.
                            auto it_alias = type_aliases_.find(sym.mangled_label);
                            if (it_alias != type_aliases_.end()) {
                                e->result_type = it_alias->second;
                                return it_alias->second;
                            }
                            // No resolvible -> void.
                            e->result_type = Type{};
                            return Type{};
                        }
                        // Variables / Constants.
                        e->result_type = sym.var_type;
                        return sym.var_type;
                    }
                }
            }
            // Limitacion G (cerrada): acceso a static field via nombre de
            // clase: @c Counter.count.  Si el base es IdentExpr cuyo nombre
            // resuelve a una clase declarada (no es una variable local),
            // tratamos como acceso a static field.  Marcamos
            // @c property_kind=3 para que el lowering emita
            // @c findclass + getstatic en vez del @c addr=obj+offset
            // habitual de instancia.
            auto it_cls_static = class_layouts_.find(base_id->name);
            if (it_cls_static != class_layouts_.end()) {
                const ClassLayout &lay = it_cls_static->second;
                for (const auto &f : lay.static_fields) {
                    if (f.name == e->field_name) {
                        e->property_kind = 3;          // marca para el lowering
                        e->result_type   = f.type;
                        return f.type;
                    }
                }
                // Nombre de clase pero el campo no es static: mensaje claro.
                diags_.error(e->loc,
                    "la clase '" + base_id->name +
                    "' no tiene un campo static llamado '" + e->field_name + "'");
                return Type{};
            }
            // L2.3: enum generico template `Maybe.None` -> resolver via
            // expected stack (LHS var-decl/param).  Sin contexto:
            // diagnostic.
            if (is_generic_enum_template(base_id->name)) {
                const std::string *expected = expected_enum_mangled(base_id->name);
                if (expected) {
                    auto it_mono = enum_layouts_.find(*expected);
                    if (it_mono != enum_layouts_.end()) {
                        const EnumLayout &elay = it_mono->second;
                        for (const auto &v : elay.variants) {
                            if (v.name == e->field_name) {
                                if (!v.field_types.empty()) {
                                    diags_.error(e->loc,
                                        std::string("variante '") + v.name +
                                        "' del enum '" + elay.name +
                                        "' tiene payload(s); usa '" + base_id->name +
                                        "." + v.name + "(...)' con argumentos");
                                    return Type{PrimitiveKind::STRUCT, elay.name};
                                }
                                e->property_kind = 99;
                                Type rt{PrimitiveKind::STRUCT, elay.name};
                                e->result_type = rt;
                                // Reescribir base_id->name al mangled
                                // para que el lowering lo trate como enum
                                // concreto.
                                base_id->name = elay.name;
                                return rt;
                            }
                        }
                    }
                }
                diags_.error(e->loc,
                    "no se puede inferir tipos para enum generico '" + base_id->name +
                    "'; usa anotacion explicita en var-decl o param");
                return Type{};
            }
            auto it_en = enum_layouts_.find(base_id->name);
            if (it_en != enum_layouts_.end()) {
                const EnumLayout &elay = it_en->second;
                for (const auto &v : elay.variants) {
                    if (v.name == e->field_name) {
                        if (!v.field_types.empty()) {
                            diags_.error(e->loc,
                                std::string("variante '") + v.name +
                                "' del enum '" + elay.name +
                                "' tiene " + std::to_string(v.field_types.size()) +
                                " payload(s); usa '" + elay.name + "." + v.name +
                                "(...)' con argumentos");
                            return Type{PrimitiveKind::STRUCT, elay.name};
                        }
                        // Variante sin payload: marcar property_kind=99
                        // para que el lowering la trate como constructor
                        // sin args.
                        e->property_kind = 99;
                        Type rt{PrimitiveKind::STRUCT, elay.name};
                        e->result_type = rt;
                        return rt;
                    }
                }
                diags_.error(e->loc,
                    "variante desconocida '" + e->field_name + "' en enum '" + elay.name + "'");
                return Type{};
            }
        }

        // Cross-module variant access: `lib.Op.Nop` o `lib.Op.Add(...)`.
        // El base (FieldAccessExpr `lib.Op`) ya resolvio a un enum type
        // (typedef alias cross-module marcado property_kind=4).  Aqui
        // detectamos que el outer FieldAccess es variant-lookup.
        if (e->base
         && e->base->kind == ast::NodeKind::FieldAccessExpr) {
            // Resolver el tipo del base primero.
            const Type bt_ns = check_expr(e->base.get());
            if (bt_ns.kind == PrimitiveKind::STRUCT) {
                auto it_en_ns = enum_layouts_.find(bt_ns.struct_name);
                if (it_en_ns != enum_layouts_.end()) {
                    const EnumLayout &elay = it_en_ns->second;
                    for (const auto &v : elay.variants) {
                        if (v.name == e->field_name) {
                            if (!v.field_types.empty()) {
                                diags_.error(e->loc,
                                    std::string("variante '") + v.name +
                                    "' del enum '" + elay.name +
                                    "' tiene " + std::to_string(v.field_types.size()) +
                                    " payload(s); usa '" + elay.name + "." + v.name +
                                    "(...)' con argumentos");
                                return Type{PrimitiveKind::STRUCT, elay.name};
                            }
                            e->property_kind = 99;  // variante sin payload
                            Type rt{PrimitiveKind::STRUCT, elay.name};
                            e->result_type = rt;
                            return rt;
                        }
                    }
                    diags_.error(e->loc,
                        "variante desconocida '" + e->field_name +
                        "' en enum '" + elay.name + "'");
                    return Type{};
                }
            }
            // No es enum cross-module: caer al path generico.
        }

        // Bajar el tipo del lado izquierdo: debe ser STRUCT o CLASS.
        const Type bt = check_expr(e->base.get());
        if (bt.kind == PrimitiveKind::STRUCT) {
            auto it = struct_layouts_.find(bt.struct_name);
            if (it == struct_layouts_.end()) {
                diags_.error(e->loc, "struct desconocido: '" + bt.struct_name + "'");
                return Type{};
            }
            const StructLayout &lay = it->second;
            for (const auto &f : lay.fields) {
                if (f.name == e->field_name) return f.type;
            }
            diags_.error(e->loc,
                "el struct '" + bt.struct_name + "' no tiene un campo llamado '" +
                e->field_name + "'");
            return Type{};
        }
        if (bt.kind == PrimitiveKind::CLASS) {
            auto it = class_layouts_.find(bt.struct_name);
            if (it == class_layouts_.end()) {
                diags_.error(e->loc, "clase desconocida: '" + bt.struct_name + "'");
                return Type{};
            }
            const ClassLayout &lay = it->second;
            // Buscar el campo y aplicar enforcement de visibilidad.  Los
            // campos privados solo se pueden acceder desde la propia
            // clase; los protegidos desde la propia o subclases.
            // Aqui solo distinguimos private vs publico/protegido porque
            // sin herencia protected actua como public.
            // Enforcement de visibilidad: si el campo es privado y se
            // accede desde fuera de la clase contenedora, error.  Como
            // StructFieldInfo no lleva el flag access, lo localizamos en
            // el AST original via @c find_class_field_access_flag.
            uint8_t access_flag = 0; // 0 = public/default
            const ast::ClassDecl *cd_orig = nullptr;
            for (auto &d : mod_.decls) {
                if (!d || d->kind != ast::NodeKind::ClassDecl) continue;
                auto *cdp = static_cast<const ast::ClassDecl *>(d.get());
                if (cdp->name == bt.struct_name) { cd_orig = cdp; break; }
            }
            if (cd_orig) {
                for (const auto &fd : cd_orig->fields) {
                    if (fd.name == e->field_name) {
                        access_flag = fd.access;
                        break;
                    }
                }
            }
            const bool inside_same_class = (current_class_ == bt.struct_name);
            if (access_flag == 1 /*private*/ && !inside_same_class) {
                diags_.error(e->loc,
                    "campo privado '" + e->field_name +
                    "' de la clase '" + bt.struct_name +
                    "' no es accesible desde fuera de la clase");
            }
            for (const auto &f : lay.fields) {
                if (f.name == e->field_name) return f.type;
            }
            for (const auto &f : lay.static_fields) {
                if (f.name == e->field_name) return f.type;
            }
            // Si no hay campo con ese nombre, buscar getter de propiedad
            // `get_<field_name>`.  Si existe, marcar como acceso de
            // propiedad y devolver su tipo de retorno.  El lowering ve
            // @c property_kind=1 y emite la llamada al accesor.
            const std::string getter_name = std::string("get_") + e->field_name;
            for (const auto &m : lay.methods) {
                if (m.name == getter_name && !m.is_constructor) {
                    e->property_kind = 1;
                    return m.return_type;
                }
            }
            // Si solo hay setter (`set_<field_name>`), el campo es
            // write-only; leerlo es error.  Distinguimos del caso
            // "no existe nada" para mejor diagnostico.
            const std::string setter_name = std::string("set_") + e->field_name;
            for (const auto &m : lay.methods) {
                if (m.name == setter_name && !m.is_constructor) {
                    diags_.error(e->loc,
                        "la propiedad '" + e->field_name +
                        "' de la clase '" + bt.struct_name +
                        "' es solo de escritura (sin getter)");
                    return Type{};
                }
            }
            diags_.error(e->loc,
                "la clase '" + bt.struct_name + "' no tiene un campo llamado '" +
                e->field_name + "'");
            return Type{};
        }
        diags_.error(e->loc,
            "el operando de '.' debe ser un struct o clase (tipo recibido: " +
            type_to_string(bt) + ")");
        return Type{};
    }

    Type TypeChecker::check_binary(ast::BinaryExpr *e) {
        const Type tl = check_expr(e->lhs.get());
        const Type tr = check_expr(e->rhs.get());

        // Operadores nativos para STRING.
        // Auto-coerce: si un lado es STRING y el otro es un literal de
        // string (PTR no-interp), el lowering lo promovera a StringObject
        // via STRMAKE.  Permite escribir "ASCII " + var sin declarar
        // variables intermedias.
        auto is_str_lit = [](ast::Expr *e) {
            if (!e || e->kind != ast::NodeKind::StringLitExpr) return false;
            auto *sl = static_cast<ast::StringLitExpr *>(e);
            return !sl->is_interpolated();
        };
        const bool lhs_str = (tl.kind == PrimitiveKind::STRING)
                          || (tl.kind == PrimitiveKind::PTR && is_str_lit(e->lhs.get()));
        const bool rhs_str = (tr.kind == PrimitiveKind::STRING)
                          || (tr.kind == PrimitiveKind::PTR && is_str_lit(e->rhs.get()));
        const bool any_real_string = (tl.kind == PrimitiveKind::STRING)
                                  || (tr.kind == PrimitiveKind::STRING);
        if (lhs_str && rhs_str && any_real_string) {
            if (e->op == ast::BinOp::Add) {
                return Type{PrimitiveKind::STRING};
            }
            if (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq) {
                return Type{PrimitiveKind::BOOL};
            }
            diags_.error(e->loc,
                "operador no soportado entre strings (solo + y == / !=)");
            return Type{};
        }

        switch (e->op) {
            case ast::BinOp::Add:
            case ast::BinOp::Sub:
            case ast::BinOp::Mul:
            case ast::BinOp::Div:
            case ast::BinOp::Mod: {
                // Aritmetica puntero (estilo C). admitimos:
                //   PTR + integer  -> PTR (escalado por sizeof(*p))
                //   PTR - integer  -> PTR
                //   PTR - PTR      -> i64 (numero de elementos entre punteros)
                // No se admite integer + PTR para evitar ambiguedad y
                // simplificar el lowering; el usuario puede escribir p + n.
                if (e->op == ast::BinOp::Add || e->op == ast::BinOp::Sub) {
                    // PTR/ARRAY + int -> PTR (decay implicito de array a ptr).
                    const bool lhs_ptr_like =
                        (tl.kind == PrimitiveKind::PTR || tl.kind == PrimitiveKind::ARRAY);
                    if (lhs_ptr_like && is_integral(tr.kind)) {
                        if (!tl.pointee
                         || tl.pointee->kind == PrimitiveKind::VOID) {
                            diags_.error(e->loc,
                                "aritmetica de punteros no permitida sobre void* o ptr sin pointee");
                            return Type{};
                        }
                        // Resultado siempre es PTR (no ARRAY), porque la
                        // aritmetica puede dejar el puntero fuera del rango
                        // del array original; el decay esta resuelto.
                        // is_virtual del resultado = is_virtual del puntero
                        // base (la aritmetica preserva la naturaleza).
                        return Type::make_ptr(*tl.pointee, tl.is_virtual);
                    }
                    // PTR - PTR -> i64 (numero de elementos entre punteros).
                    // Tambien aceptamos ARRAY mediante decay.
                    if (e->op == ast::BinOp::Sub) {
                        const bool rhs_ptr_like =
                            (tr.kind == PrimitiveKind::PTR || tr.kind == PrimitiveKind::ARRAY);
                        if (lhs_ptr_like && rhs_ptr_like) {
                            if (!tl.pointee || !tr.pointee
                             || *tl.pointee != *tr.pointee) {
                                diags_.error(e->loc,
                                    "p - q requiere punteros al mismo tipo");
                                return Type{};
                            }
                            return Type{PrimitiveKind::I64};
                        }
                    }
                }
                if (!is_numeric(tl.kind) || !is_numeric(tr.kind)) {
                    diags_.error(e->loc, "operandos no numericos en operacion aritmetica");
                    return Type{};
                }
                if (e->op == ast::BinOp::Mod) {
                    if (!is_integral(tl.kind) || !is_integral(tr.kind)) {
                        diags_.error(e->loc, "'%' requiere operandos enteros");
                        return Type{};
                    }
                }
                /* Narrowing de literales enteros 2026-05-16: si UN operando es
                 * un IntLit (default i64) y el otro tiene un tipo entero
                 * estrecho (i32/i16/i8/u32/u16/u8), y el literal CABE en ese
                 * tipo, narrow el literal al tipo del otro.  Esto evita que
                 * `i32 + 1` se baje como sext.i64 + add.i64 + trunc.i32
                 * cuando puede ser add.i32 directo.
                 *
                 * Beneficio: el hot loop tipico (counter i32 += 1) baja a
                 * `add.i32` puro en lugar de la cadena sext+add+trunc.  Cada
                 * iteracion elimina 4 instrucciones IR (-> 4-7 instr maquina).
                 */
                auto narrow_int_lit_to = [&](ast::Expr *operand,
                                              PrimitiveKind target) -> bool {
                    if (!operand) return false;
                    if (operand->kind != ast::NodeKind::IntLitExpr) return false;
                    if (!is_integral(target)) return false;
                    auto *il = static_cast<ast::IntLitExpr *>(operand);
                    const int64_t v = static_cast<int64_t>(il->value);
                    bool fits = false;
                    switch (target) {
                        case PrimitiveKind::I8:
                            fits = v >= INT8_MIN && v <= INT8_MAX; break;
                        case PrimitiveKind::I16:
                            fits = v >= INT16_MIN && v <= INT16_MAX; break;
                        case PrimitiveKind::I32:
                            fits = v >= INT32_MIN && v <= INT32_MAX; break;
                        case PrimitiveKind::I64:
                            fits = true; break;
                        case PrimitiveKind::U8:
                            fits = v >= 0 && v <= 0xFF; break;
                        case PrimitiveKind::U16:
                            fits = v >= 0 && v <= 0xFFFF; break;
                        case PrimitiveKind::U32:
                            fits = v >= 0 && static_cast<uint64_t>(v) <= 0xFFFFFFFFULL; break;
                        case PrimitiveKind::U64:
                            fits = v >= 0; break;
                        default:
                            return false;
                    }
                    if (!fits) return false;
                    /* Modificar el result_type del literal in-place. */
                    il->result_type = Type{target};
                    return true;
                };
                Type adj_l = tl;
                Type adj_r = tr;
                /* Si lhs es IntLit y rhs es entero estrecho que lo contiene. */
                if (is_integral(tl.kind) && is_integral(tr.kind)
                 && tl.kind != tr.kind) {
                    if (narrow_int_lit_to(e->lhs.get(), tr.kind)) {
                        adj_l = tr;
                    } else if (narrow_int_lit_to(e->rhs.get(), tl.kind)) {
                        adj_r = tl;
                    }
                }
                return Type{promote_arith(adj_l.kind, adj_r.kind)};
            }
            case ast::BinOp::Eq:
            case ast::BinOp::Neq:
            case ast::BinOp::Lt:
            case ast::BinOp::Le:
            case ast::BinOp::Gt:
            case ast::BinOp::Ge: {
                // Comparaciones de punteros (PTR vs PTR) tratan los punteros
                // como uint64.  PTR vs null (void*) admitido tambien.
                if (tl.kind == PrimitiveKind::PTR && tr.kind == PrimitiveKind::PTR) {
                    return Type{PrimitiveKind::BOOL};
                }
                // Comparacion de referencias CLASS contra null o entre si.
                // El lowering trata las refs CLASS como i64 (puntero al
                // ObjectHeader), asi que la comparacion se hace tambien
                // como entero.  Solo permitimos == y !=, no < <= > >=.
                const bool both_class_or_null =
                    (tl.kind == PrimitiveKind::CLASS || tl.kind == PrimitiveKind::PTR)
                 && (tr.kind == PrimitiveKind::CLASS || tr.kind == PrimitiveKind::PTR);
                if (both_class_or_null
                 && (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq)) {
                    return Type{PrimitiveKind::BOOL};
                }
                // Bug fix 2026-05-23 (LR2): struct value-type == struct
                // value-type via comparacion campo-a-campo.  Solo permitido
                // si ambos lados son del MISMO struct nombrado.  Los structs
                // con bit fields se comparan correctamente porque el lowering
                // hace una secuencia de loads + cmp_eq + and.  Solo == y !=.
                if (tl.kind == PrimitiveKind::STRUCT
                 && tr.kind == PrimitiveKind::STRUCT
                 && tl.struct_name == tr.struct_name
                 && !tl.struct_name.empty()
                 && (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq)) {
                    return Type{PrimitiveKind::BOOL};
                }
                // Bug fix 2026-05-23: char es un codepoint entero (UTF-32);
                // se compara con int como integral (igual que C).  Tratar
                // char como numerico SOLO en este contexto de comparacion.
                auto is_cmp_compatible = [](PrimitiveKind k) {
                    return is_numeric(k) || k == PrimitiveKind::CHAR;
                };
                if (!is_cmp_compatible(tl.kind) || !is_cmp_compatible(tr.kind)) {
                    // Permitimos bool == bool; resto de combinaciones invalidas.
                    if (!(tl.kind == PrimitiveKind::BOOL && tr.kind == PrimitiveKind::BOOL)) {
                        diags_.error(e->loc, "operandos no numericos en comparacion");
                    }
                }
                return Type{PrimitiveKind::BOOL};
            }
            case ast::BinOp::LogicalAnd:
            case ast::BinOp::LogicalOr: {
                // Aceptamos bool y numericos (estilo C).
                if (tl.kind != PrimitiveKind::BOOL && !is_numeric(tl.kind)) {
                    diags_.error(e->loc, "operando izquierdo no booleano en operador logico");
                }
                if (tr.kind != PrimitiveKind::BOOL && !is_numeric(tr.kind)) {
                    diags_.error(e->loc, "operando derecho no booleano en operador logico");
                }
                return Type{PrimitiveKind::BOOL};
            }
            case ast::BinOp::BitAnd:
            case ast::BinOp::BitOr:
            case ast::BinOp::BitXor:
            case ast::BinOp::Shl:
            case ast::BinOp::Shr: {
                if (!is_integral(tl.kind) || !is_integral(tr.kind)) {
                    diags_.error(e->loc, "operandos no enteros en operacion bitwise");
                    return Type{};
                }
                return Type{promote_arith(tl.kind, tr.kind)};
            }
        }
        return Type{};
    }

    Type TypeChecker::check_unary(ast::UnaryExpr *e) {
        const Type t = check_expr(e->operand.get());
        switch (e->op) {
            case ast::UnOp::Neg:
            case ast::UnOp::Pos:
                if (!is_numeric(t.kind)) {
                    diags_.error(e->loc, "operador unario aritmetico requiere numerico");
                }
                return t;
            case ast::UnOp::LogicalNot:
                return Type{PrimitiveKind::BOOL};
            case ast::UnOp::Unwrap:
                // !!x assert non-null: requiere referencia (CLASS o PTR).
                // Lowering identico a unwrap(x) pero sintactico-mas-corto.
                if (t.kind != PrimitiveKind::CLASS
                 && t.kind != PrimitiveKind::PTR
                 && t.kind != PrimitiveKind::COUNT) {
                    diags_.error(e->loc,
                        std::string("'!!' requiere una referencia, recibido ")
                        + type_to_string(t));
                }
                return t;
            case ast::UnOp::Await: {
                // Mejora II: `await fut` extrae el tipo logico T de
                // Future<T>.  Casos aceptados:
                //   - Future<T>: devuelve T (el frontend hace cast/bitcast
                //     adecuado al lowering).
                //   - i64/i32/u64/u32 (legacy): devuelve I64 sin cast.
                //     Util para handles raw alocados via `future` directo.
                //   - COUNT (tipo desconocido): devuelve I64 default.
                if (t.kind == PrimitiveKind::FUTURE) {
                    if (t.pointee) return *t.pointee;
                    return Type{PrimitiveKind::I64};
                }
                if (t.kind != PrimitiveKind::I64
                 && t.kind != PrimitiveKind::I32
                 && t.kind != PrimitiveKind::U64
                 && t.kind != PrimitiveKind::U32
                 && t.kind != PrimitiveKind::COUNT) {
                    diags_.error(e->loc,
                        std::string("'await' requiere Future<T> o handle i64, recibido ")
                        + type_to_string(t));
                }
                return Type{PrimitiveKind::I64};
            }
            case ast::UnOp::BitNot:
                if (!is_integral(t.kind)) {
                    diags_.error(e->loc, "'~' requiere operando entero");
                }
                return t;
            case ast::UnOp::PreInc:
            case ast::UnOp::PreDec:
            case ast::UnOp::PostInc:
            case ast::UnOp::PostDec:
                if (!is_integral(t.kind)) {
                    diags_.error(e->loc, "++/-- requieren operando entero");
                }
                // bug4: aceptar IdentExpr (var local), FieldAccessExpr
                // (this.x, obj.x), IndexExpr (arr[i]) y UnaryExpr Deref
                // (*p) como lvalues validos para ++/--.
                if (e->operand) {
                    const auto k = e->operand->kind;
                    const bool is_lvalue =
                        k == ast::NodeKind::IdentExpr
                     || k == ast::NodeKind::FieldAccessExpr
                     || k == ast::NodeKind::IndexExpr
                     || (k == ast::NodeKind::UnaryExpr
                         && static_cast<ast::UnaryExpr *>(e->operand.get())->op
                            == ast::UnOp::Deref);
                    if (!is_lvalue) {
                        diags_.error(e->loc, "++/-- requieren un lvalue");
                    }
                }
                return t;
            case ast::UnOp::AddrOf: {
                // '&x' requiere un lvalue.  aceptamos:
                //  - IdentExpr (variable local; el lowering la promociona
                //    a ALLOCA si todavia no lo estaba).
                //  - FieldAccessExpr (campo de un struct; ya es address-taken).
                //  - UnaryExpr(Deref, p) -> equivalente al propio p.
                // Otros casos (e.g. literales, expresiones temporales) son
                // errores: no hay direccion estable.
                if (!e->operand) {
                    diags_.error(e->loc, "'&' requiere un operando");
                    return Type{};
                }
                const auto kind = e->operand->kind;
                const bool is_lvalue =
                    kind == ast::NodeKind::IdentExpr
                 || kind == ast::NodeKind::FieldAccessExpr
                 || kind == ast::NodeKind::IndexExpr
                 || (kind == ast::NodeKind::UnaryExpr
                     && static_cast<ast::UnaryExpr *>(e->operand.get())->op
                        == ast::UnOp::Deref);
                if (!is_lvalue) {
                    diags_.error(e->loc,
                        "'&' requiere un lvalue (variable, campo, p[i] o *p)");
                    return Type{};
                }
                // `&x` siempre devuelve VirtualPtr<T>: la direccion es del
                // stack VM (locales) o del payload de un objeto GC (que
                // tambien vive en host -- ver caso 1 abajo).
                //
                // Caso 1: campo de un objeto CLASS o STRUCT.  Si el operando
                // es FieldAccessExpr cuya base es CLASS o STRUCT alocado
                // en host (e.g. via @c new), la direccion del campo es
                // HOST.  Lo distinguimos por el tipo del operando: si el
                // base.result_type es PTR is_virtual=false (host) o CLASS,
                // entonces la direccion del campo tambien es host.
                bool result_is_virtual = true;
                if (kind == ast::NodeKind::FieldAccessExpr) {
                    auto *fa = static_cast<ast::FieldAccessExpr *>(e->operand.get());
                    const Type bt = fa->base ? fa->base->result_type : Type{};
                    if (bt.kind == PrimitiveKind::CLASS) {
                        result_is_virtual = false;   // CLASS payload vive en GC heap (host)
                    } else if (bt.kind == PrimitiveKind::PTR && !bt.is_virtual) {
                        result_is_virtual = false;   // ptr host -> field es host
                    }
                    // STRUCT local sigue en stack VM por default (virtual).
                }
                // Caso 2: subscript (p[i]).  La direccion del elemento
                // hereda la naturaleza del puntero base.
                if (kind == ast::NodeKind::IndexExpr) {
                    auto *ie = static_cast<ast::IndexExpr *>(e->operand.get());
                    const Type bt = ie->base ? ie->base->result_type : Type{};
                    if (bt.kind == PrimitiveKind::PTR && !bt.is_virtual) {
                        result_is_virtual = false;
                    }
                    // ARRAY local: virtual.  ARRAY host: virtual=false.
                    if (bt.kind == PrimitiveKind::ARRAY && !bt.is_virtual) {
                        result_is_virtual = false;
                    }
                }
                // Caso 3: deref (*p).  &*p == p; preserva exactamente la
                // naturaleza del puntero original.
                if (kind == ast::NodeKind::UnaryExpr) {
                    auto *un = static_cast<ast::UnaryExpr *>(e->operand.get());
                    if (un->operand) {
                        const Type pt = un->operand->result_type;
                        if (pt.kind == PrimitiveKind::PTR) {
                            result_is_virtual = pt.is_virtual;
                        }
                    }
                }
                return Type::make_ptr(t, result_is_virtual);
            }
            case ast::UnOp::Deref: {
                // '*p' requiere que p sea un puntero; el tipo resultante
                // es el del tipo apuntado.  Desreferenciar void (resultado
                // de un pointee no resuelto) emite error.
                if (t.kind != PrimitiveKind::PTR || !t.pointee) {
                    diags_.error(e->loc,
                        std::string("'*' requiere un puntero, recibido ")
                        + type_to_string(t));
                    return Type{};
                }
                if (t.pointee->kind == PrimitiveKind::VOID) {
                    diags_.error(e->loc,
                        "'*' no puede desreferenciar un puntero a void");
                    return Type{};
                }
                return *t.pointee;
            }
        }
        return Type{};
    }

    Type TypeChecker::check_assign(ast::AssignExpr *e) {
        // Target debe ser un lvalue.  admitimos:
        //  - IdentExpr        (variable simple).
        //  - FieldAccessExpr  (p.x = v).
        // Otros lvalues (deref de puntero, indexado de array) llegaran
        if (!e->target) {
            diags_.error(e->loc, "el lado izquierdo de '=' es nulo");
            (void)check_expr(e->value.get());
            return Type{};
        }

        // validacion de escape ilegal para clases con destructor.
        // Si el value es una instancia de clase con `~Class()` y se asigna
        // a un field, slot de array o deref-store, el destructor RAII del
        // nunca llegara a ejecutarse (no hay scope owner).  Mejor
        // rechazar en compile time con error claro que dejar leaks
        // silenciosos del recurso wrapped.
        //
        // Casos legales (NO se rechazan aqui):
        //   - target IdentExpr (asignacion a var local): el cleanup_stack_
        //     ejecuta el destructor al exit del scope.
        //   - return res; (handled por escape detection -- caller
        //     toma owner via su propio cleanup_stack_).
        //   - dispose(res) explicito 
        //
        // Caso ilegal:
        //   - this.field   = res;  (atributo de objeto: el dtor del objeto
        //                           no recursa a sus fields todavia)
        //   - obj.field    = res;  idem
        //   - arr[i]       = res;  (slot de array nativo, sin cleanup)
        //   - *p           = res;  (deref store, sin cleanup)
        if (e->target && e->value) {
            const bool target_is_field = (e->target->kind == ast::NodeKind::FieldAccessExpr);
            const bool target_is_index = (e->target->kind == ast::NodeKind::IndexExpr);
            bool target_is_deref = false;
            if (e->target->kind == ast::NodeKind::UnaryExpr) {
                auto *u = static_cast<ast::UnaryExpr *>(e->target.get());
                if (u->op == ast::UnOp::Deref) target_is_deref = true;
            }
            if (target_is_field || target_is_index || target_is_deref) {
                // Resolver tipo del value sin reportar errores de check_expr
                // para no duplicar diagnosticos.  Si es CLASS con destructor,
                // emitir error claro.
                Type tv_peek = check_expr(e->value.get());
                e->value->result_type = tv_peek;
                if (tv_peek.kind == PrimitiveKind::CLASS) {
                    auto it_cls = class_layouts_.find(tv_peek.struct_name);
                    if (it_cls != class_layouts_.end() && it_cls->second.has_destructor) {
                        // relajacion: si el target es un FieldAccess y
                        // la CLASE CONTENEDORA tambien es destructible (tiene
                        // su propio destructor o has_destructible_field), la
                        // asignacion es legal.  El lowering augmenta el
                        // destructor del contenedor para que invoque el
                        // destructor del field automaticamente al ser
                        // destruido (RAII recursivo).
                        bool container_owns = false;
                        if (target_is_field) {
                            auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
                            if (fa && fa->base) {
                                Type tb = check_expr(fa->base.get());
                                if (tb.kind == PrimitiveKind::CLASS) {
                                    auto it_outer = class_layouts_.find(tb.struct_name);
                                    if (it_outer != class_layouts_.end()
                                     && (it_outer->second.has_destructor
                                      || it_outer->second.has_destructible_field)) {
                                        container_owns = true;
                                    }
                                }
                            }
                        }
                        if (!container_owns) {
                            const char *target_name =
                                target_is_field ? "campo de objeto/struct"
                              : target_is_index ? "slot de array"
                                                : "deref de puntero";
                            diags_.error(e->loc,
                                std::string("clase '") + tv_peek.struct_name +
                                "' tiene destructor `~" + tv_peek.struct_name +
                                "()` y no puede asignarse a " + target_name +
                                ": el destructor solo se ejecuta automaticamente "
                                "para variables locales (RAII).  Usa `return`, "
                                "`dispose(...)` o vive en una variable local. "
                                "(O bien declara un destructor en la clase "
                                "contenedora para que herede la responsabilidad RAII.)");
                            return Type{PrimitiveKind::VOID};
                        }
                    }
                }
                // Re-procesamos el target/value despues por el flujo normal.
                // Idempotente porque check_expr no muta el AST de forma no-op.
            }
        }
        // Caso FieldAccessExpr: validar como lvalue de struct/clase, o
        // detectar setter de propiedad si la clase tiene `set_<field>`.
        if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
            // Limitacion (cerrada): @c Counter.count = v.  Si el base
            // es IdentExpr cuyo nombre es una clase declarada, tratamos
            // como asignacion a static field y delegamos a
            // @c check_field_access (que marca property_kind=3 y resuelve
            // el tipo desde @c lay.static_fields).  Sin esto, el
            // @c check_expr(base) reportaria "nombre no declarado".
            if (fa->base
             && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *base_id = static_cast<ast::IdentExpr *>(fa->base.get());
                auto it_cls = class_layouts_.find(base_id->name);
                if (it_cls != class_layouts_.end()) {
                    Type ft_static = check_field_access(fa);
                    fa->result_type = ft_static;
                    const Type tv = check_expr(e->value.get());
                    if (ft_static.kind != PrimitiveKind::COUNT
                     && tv.kind != PrimitiveKind::COUNT
                     && !types_assignable(ft_static, tv)) {
                        diags_.error(e->loc,
                            std::string("tipo del valor (") + type_to_string(tv) +
                            ") incompatible con tipo del static field '" +
                            fa->field_name + "' (" + type_to_string(ft_static) + ")");
                    }
                    return ft_static;
                }
            }

            // chequeo previo de setter de propiedad.  Solo aplica
            // si el base resuelve a CLASS y la clase tiene un metodo
            // `set_<field_name>` (con parametro tipado).  En ese caso
            // marcamos @c property_kind=2 y devolvemos el tipo del
            // parametro como tipo del lvalue.
            const Type bt_pre = check_expr(fa->base.get());
            fa->base->result_type = bt_pre;
            if (bt_pre.kind == PrimitiveKind::CLASS) {
                auto itc = class_layouts_.find(bt_pre.struct_name);
                if (itc != class_layouts_.end()) {
                    const ClassLayout &lay = itc->second;
                    bool has_field = false;
                    for (const auto &f : lay.fields) {
                        if (f.name == fa->field_name) { has_field = true; break; }
                    }
                    for (const auto &f : lay.static_fields) {
                        if (f.name == fa->field_name) { has_field = true; break; }
                    }
                    if (!has_field) {
                        const std::string setter_name = std::string("set_") + fa->field_name;
                        const ClassMethodInfo *setter = nullptr;
                        for (const auto &m : lay.methods) {
                            if (m.name == setter_name && !m.is_constructor) {
                                setter = &m;
                                break;
                            }
                        }
                        if (setter) {
                            fa->property_kind = 2;
                            const Type pt = setter->param_types.empty()
                                ? Type{} : setter->param_types.front();
                            fa->result_type = pt;
                            const Type tv = check_expr(e->value.get());
                            if (tv.kind != PrimitiveKind::COUNT
                             && pt.kind != PrimitiveKind::COUNT
                             && !types_assignable(pt, tv)) {
                                diags_.error(e->loc,
                                    std::string("tipo del valor (") + type_to_string(tv) +
                                    ") incompatible con tipo del setter (" + type_to_string(pt) + ")");
                            }
                            return pt;
                        }
                        // Si solo hay getter, el campo es read-only.
                        const std::string getter_name = std::string("get_") + fa->field_name;
                        for (const auto &m : lay.methods) {
                            if (m.name == getter_name && !m.is_constructor) {
                                diags_.error(e->loc,
                                    "la propiedad '" + fa->field_name +
                                    "' de la clase '" + bt_pre.struct_name +
                                    "' es solo de lectura (sin setter)");
                                (void)check_expr(e->value.get());
                                return Type{};
                            }
                        }
                    }
                }
            }
            // Camino normal: campo de struct/clase.  El check_field_access
            // re-evalua base() pero el lookup esta cacheado en su layout.
            Type ft = check_field_access(fa);
            fa->result_type = ft;
            const Type tv = check_expr(e->value.get());
            // null asignable a cualquier referencia CLASS (modelo
            // nullable por defecto, igual que en check_var_decl).
            const bool null_to_class_field =
                e->value
             && e->value->kind == ast::NodeKind::NullLitExpr
             && ft.kind == PrimitiveKind::CLASS;
            // Tambien admitimos asignacion de instancia de subclase a
            // campo declarado como interfaz/superclase via class_is_assignable.
            if (tv.kind != PrimitiveKind::COUNT
             && !types_assignable(ft, tv)
             && !class_is_assignable(ft, tv)
             && !null_to_class_field) {
                diags_.error(e->loc,
                    std::string("tipo del valor (") + type_to_string(tv) +
                    ") incompatible con tipo del campo (" + type_to_string(ft) + ")");
            }
            return ft;
        }
        // Caso IndexExpr: 'p[i] = v' equivale a *(p+i) = v; el tipo es
        // el del pointee y aplicamos las mismas reglas de compatibilidad
        // que para Deref.
        if (e->target->kind == ast::NodeKind::IndexExpr) {
            auto *ix = static_cast<ast::IndexExpr *>(e->target.get());
            const Type tt = check_index(ix);
            ix->result_type = tt;
            const Type tv = check_expr(e->value.get());
            // BugFix P1-B1: ademas de types_assignable (igualdad estricta o
            // coercion numerica), aceptar subtipado CLASS<->Interface via
            // class_is_assignable.  Tambien aceptar null literal asignable
            // a array de CLASS/STRING/PTR.
            const bool null_ok =
                (tt.kind == PrimitiveKind::CLASS
                 || tt.kind == PrimitiveKind::STRING
                 || tt.kind == PrimitiveKind::PTR)
                && tv.kind == PrimitiveKind::PTR
                && (!tv.pointee || tv.pointee->kind == PrimitiveKind::VOID);
            if (tt.kind != PrimitiveKind::COUNT
             && tv.kind != PrimitiveKind::COUNT
             && !types_assignable(tt, tv)
             && !class_is_assignable(tt, tv)
             && !null_ok) {
                diags_.error(e->loc,
                    std::string("tipo del valor (") + type_to_string(tv) +
                    ") incompatible con tipo del elemento (" + type_to_string(tt) + ")");
            }
            return tt;
        }
        // Caso UnaryExpr(Deref, p): '*p = v' escribe a traves del puntero.
        // Validamos que p sea un puntero a un tipo asignable y que el tipo
        // del valor encaje con el pointee.
        if (e->target->kind == ast::NodeKind::UnaryExpr) {
            auto *un = static_cast<ast::UnaryExpr *>(e->target.get());
            if (un->op == ast::UnOp::Deref) {
                const Type tt = check_unary(un); // valida el deref y devuelve pointee
                un->result_type = tt;
                const Type tv = check_expr(e->value.get());
                if (tt.kind != PrimitiveKind::COUNT
                 && tv.kind != PrimitiveKind::COUNT
                 && !types_assignable(tt, tv)) {
                    diags_.error(e->loc,
                        std::string("tipo del valor (") + type_to_string(tv) +
                        ") incompatible con tipo apuntado (" + type_to_string(tt) + ")");
                }
                return tt;
            }
        }
        if (e->target->kind != ast::NodeKind::IdentExpr) {
            diags_.error(e->loc,
                "el lado izquierdo de '=' debe ser un identificador o un acceso a campo");
            (void)check_expr(e->value.get());
            return Type{};
        }
        const auto *id = static_cast<const ast::IdentExpr *>(e->target.get());
        /* asignacion a comptime var (mutable).  Si el nombre esta
         * en el stack de comptime const locales y is_mutable=true,
         * permitimos la asignacion: el comptime_eval_stmt (en bloques
         * comptime / fn bodies) la procesa.  No emitimos error aqui. */
        {
            const auto &stack = comptime_const_locals_;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                auto hit = it->find(id->name);
                if (hit != it->end()) {
                    if (!hit->second.is_mutable) {
                        diags_.error(e->loc,
                            "asignacion a 'comptime const' inmutable: '"
                            + id->name + "'");
                        return Type{};
                    }
                    /* Chequear tipo del valor (compatible). */
                    Type vt = check_expr(e->value.get());
                    (void)vt;
                    return hit->second.type;
                }
            }
        }
        size_t id_depth = 0;
        const Symbol *s = lookup_with_depth(id->name, &id_depth);
        if (!s) {
            diags_.error(e->loc, "nombre no declarado: '" + id->name + "'");
            (void)check_expr(e->value.get());
            return Type{};
        }
        if (s->kind == SymbolKind::Function) {
            diags_.error(e->loc, "no se puede asignar a una funcion");
            (void)check_expr(e->value.get());
            return Type{};
        }
        if (s->is_const) {
            diags_.error(e->loc, "asignacion a variable 'const': '" + id->name + "'");
        }
        // closures: si la asignacion ocurre DENTRO de una lambda y
        // el target es una variable del scope EXTERIOR, marcar la
        // captura como mutable: el lowering la promovera a address-taken
        // (ALLOCA estable en el scope outer) y guardara su PUNTERO en
        // el env block, no su valor.  Asi reads/writes desde el body de
        // la lambda se ven correctamente fuera.  Sin esto, la asignacion
        // dentro del helper modifica solo el SSA value local y NO
        // propaga al outer.
        if (!lambda_stack_.empty()) {
            for (auto &ctx : lambda_stack_) {
                if (id_depth < ctx.outer_depth) {
                    bool already = false;
                    for (auto &nm : ctx.expr->captures) {
                        if (nm == id->name) { already = true; break; }
                    }
                    if (!already) {
                        ctx.expr->captures.push_back(id->name);
                        ctx.expr->capture_types.push_back(s->type);
                    }
                    // Marcar la captura como mutable.  Si ya esta en
                    // mutable_captures, no duplicamos.
                    bool already_mut = false;
                    for (auto &nm : ctx.expr->mutable_captures) {
                        if (nm == id->name) { already_mut = true; break; }
                    }
                    if (!already_mut) {
                        ctx.expr->mutable_captures.push_back(id->name);
                    }
                }
            }
        }
        // Marcar el tipo del target en el AST.
        e->target->result_type = s->type;

        // Sprint edge-bugs (2026-06-02): si el target tiene tipo FUNCTION y
        // el value es una LambdaExpr sin annotations de tipo, propagar la
        // firma esperada (params + return_type) a la lambda antes de
        // check_expr.  Sin esto, la lambda infiere VOID como return y
        // dispara 'return con valor en funcion declarada void' en su body.
        // Mismo patron que check_var_decl + check_return ya hacen.
        if (s->type.kind == PrimitiveKind::FUNCTION
         && e->value && e->value->kind == ast::NodeKind::LambdaExpr) {
            auto *lam = static_cast<ast::LambdaExpr *>(e->value.get());
            if (lam->params.size() == s->type.fn_params.size()) {
                for (size_t i = 0; i < lam->params.size(); ++i) {
                    if (!lam->params[i]->type) {
                        const Type &pt = s->type.fn_params[i];
                        if (pt.kind == PrimitiveKind::CLASS
                         || pt.kind == PrimitiveKind::STRUCT) {
                            auto nt = std::make_unique<ast::NamedTypeNode>();
                            nt->loc  = lam->params[i]->loc;
                            nt->name = pt.struct_name;
                            lam->params[i]->type = std::move(nt);
                        } else {
                            auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                            pn->loc  = lam->params[i]->loc;
                            pn->prim = pt.kind;
                            lam->params[i]->type = std::move(pn);
                        }
                    }
                }
                if (!lam->return_type && s->type.pointee) {
                    const Type &rt = *s->type.pointee;
                    if (rt.kind == PrimitiveKind::CLASS
                     || rt.kind == PrimitiveKind::STRUCT) {
                        auto nt = std::make_unique<ast::NamedTypeNode>();
                        nt->loc  = lam->loc;
                        nt->name = rt.struct_name;
                        lam->return_type = std::move(nt);
                    } else {
                        auto pn = std::make_unique<ast::PrimitiveTypeNode>();
                        pn->loc  = lam->loc;
                        pn->prim = rt.kind;
                        lam->return_type = std::move(pn);
                    }
                }
            }
        }

        const Type tv = check_expr(e->value.get());
        if (tv.kind != PrimitiveKind::COUNT && !types_assignable(s->type, tv)) {
            diags_.error(e->loc,
                std::string("tipo del valor (") + type_to_string(tv) +
                ") incompatible con tipo del destino (" + type_to_string(s->type) + ")");
        }
        return s->type;
    }

    Type TypeChecker::check_call(ast::CallExpr *e) {
        if (!e->callee) {
            diags_.error(e->loc, "callee nulo en llamada");
            for (auto &a : e->args) (void)check_expr(a.get());
            return Type{};
        }

        // Metodos OO sobre tipo string.  Si callee es
        // FieldAccess con base STRING (NO un enum identifier) y nombre
        // de metodo conocido, devolvemos el tipo del builtin equivalente.
        // CRITICO: el check enum constructor `Color.Red(...)` esta mas
        // abajo y necesita la base intacta.  Skip si la base es un
        // IdentExpr que nombra un enum.
        if (e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            // Phase M.7: namespace.function(args) -- el base resuelve a
            // Symbol::Namespace y el field es una funcion del namespace.
            // Detectar ANTES de check_expr(base) para que no se trate
            // como variable indefinida.
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *idb = static_cast<ast::IdentExpr *>(fa->base.get());
                const Symbol *ns_sym = lookup(idb->name);
                if (ns_sym && ns_sym->kind == SymbolKind::Namespace) {
                    // L.26 fix (2026-06-04): marcar el namespace como
                    // referenciado para que el linter de "import no se
                    // usa" no genere falsos positivos en namespace calls
                    // (`ns.fn()`).  Sin esto, los warnings spurios
                    // aparecen aunque el usuario claramente usa el
                    // import via call.  Mismo patron que check_field_access
                    // ya hacia para namespace field access.
                    referenced_names_.insert(idb->name);
                    if (ns_sym->ns_index < imported_namespaces_.size()) {
                        const auto &ns = imported_namespaces_[ns_sym->ns_index];
                        auto its = ns.by_name.find(fa->field_name);
                        if (its == ns.by_name.end()) {
                            diags_.error(e->loc,
                                "el namespace '" + idb->name +
                                "' no tiene un simbolo llamado '" +
                                fa->field_name + "'");
                            for (auto &a : e->args) (void)check_expr(a.get());
                            return Type{};
                        }
                        const auto &sym = ns.symbols[its->second];
                        // Phase M.7.c: si la sig esta vacia (namespace
                        // inline; las firmas se rellenan en check_function),
                        // buscamos la sig real via function_sig_by_name
                        // usando el mangled_label.  Para namespaces
                        // cross-module (M7.a), sym.sig ya esta lleno.
                        const FunctionSig *real_sig = nullptr;
                        if (!sym.mangled_label.empty()) {
                            real_sig = function_sig_by_name(sym.mangled_label);
                        }
                        const FunctionSig *use_sig = real_sig
                            ? real_sig
                            : &sym.sig;
                        // Validar aridad.
                        if (e->args.size() != use_sig->param_types.size()) {
                            diags_.error(e->loc,
                                "llamada a '" + idb->name + "." +
                                fa->field_name + "': se esperaban " +
                                std::to_string(use_sig->param_types.size()) +
                                " args, recibidos " +
                                std::to_string(e->args.size()));
                        }
                        // Chequear cada arg (sin validacion estricta de
                        // tipo en MVP; M7.x anyadira coerce + cast checks).
                        for (auto &a : e->args) (void)check_expr(a.get());
                        // Marcar el FieldAccess para que el lowering lo
                        // reconozca como namespace call y emita CALLVM al
                        // mangled_label.
                        fa->property_kind = 4;
                        fa->ns_index      = ns_sym->ns_index;  // M.7
                        fa->result_type   = Type::make_function(
                            use_sig->param_types, use_sig->return_type);
                        e->result_type = use_sig->return_type;
                        return use_sig->return_type;
                    }
                }
                auto it_cls_s = class_layouts_.find(idb->name);
                if (it_cls_s != class_layouts_.end()
                 && lookup(idb->name) == nullptr) {
                    const ClassLayout &cls = it_cls_s->second;
                    const ClassMethodInfo *smtd = nullptr;
                    for (const auto &m : cls.methods) {
                        if (m.is_constructor) continue;
                        if (m.is_static && m.name == fa->field_name) {
                            smtd = &m; break;
                        }
                    }
                    if (smtd) {
                        if (e->args.size() != smtd->param_types.size()) {
                            diags_.error(e->loc,
                                idb->name + "." + fa->field_name +
                                ": numero de argumentos incorrecto (esperado " +
                                std::to_string(smtd->param_types.size()) +
                                ", recibido " + std::to_string(e->args.size()) + ")");
                        }
                        for (size_t i = 0; i < e->args.size(); ++i) {
                            Type at = check_expr(e->args[i].get());
                            if (i < smtd->param_types.size()
                             && !types_assignable(smtd->param_types[i], at)
                             && at.kind != PrimitiveKind::COUNT) {
                                diags_.error(e->loc,
                                    idb->name + "." + fa->field_name +
                                    ": arg " + std::to_string(i+1) +
                                    " tipo (" + type_to_string(at) +
                                    ") incompatible con (" +
                                    type_to_string(smtd->param_types[i]) + ")");
                            }
                        }
                        fa->property_kind = 4;
                        fa->base->result_type = Type{PrimitiveKind::VOID};
                        e->result_type = smtd->return_type;
                        return smtd->return_type;
                    }
                }
            }
            bool base_is_enum_id = false;
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *id_b = static_cast<ast::IdentExpr *>(fa->base.get());
                if (enum_layouts_.find(id_b->name) != enum_layouts_.end()) {
                    base_is_enum_id = true;
                }
                // L2.3: enums genericos templates.
                if (is_generic_enum_template(id_b->name)) {
                    base_is_enum_id = true;
                }
            }
            if (!base_is_enum_id) {
                Type base_t = check_expr(fa->base.get());
                fa->base->result_type = base_t;
                if (base_t.kind == PrimitiveKind::STRING) {
                    static const struct { const char *m; PrimitiveKind ret; } MAP[] = {
                        {"length", PrimitiveKind::I64},
                        {"bytes",  PrimitiveKind::I64},
                        {"cstr",   PrimitiveKind::PTR},
                        {"wstr",   PrimitiveKind::PTR},
                        {"hash",   PrimitiveKind::U64},
                        {"intern", PrimitiveKind::STRING},
                        {"equals", PrimitiveKind::BOOL},
                        {"concat", PrimitiveKind::STRING},
                    };
                    for (const auto &m : MAP) {
                        if (fa->field_name == m.m) {
                            for (auto &a : e->args) (void)check_expr(a.get());
                            return Type{m.ret};
                        }
                    }
                }
                // dispatch de metodos de coleccion primitiva.  Si el
                // base es uno de los tipos coleccion (ARRAYLIST/HASHMAP/etc),
                // buscamos el metodo en la tabla COL_METHODS y devolvemos
                // su tipo de retorno.  Validamos aridad simple; tipos de
                // arg los chequeamos via check_expr(a) sin cast (todos los
                // args nativos son uint64_t en la calling convention CALLN).
                if (is_col_kind(base_t.kind)) {
                    const ColMethod *cm = find_col_method(base_t.kind, fa->field_name);
                    if (cm) {
                        if ((int)e->args.size() != cm->n_args) {
                            diags_.error(e->loc,
                                std::string("'") + primitive_name(base_t.kind) +
                                "." + cm->vex_name + "' espera " +
                                std::to_string(cm->n_args) + " arg(s), recibidos " +
                                std::to_string(e->args.size()));
                        }
                        for (auto &a : e->args) (void)check_expr(a.get());
                        return Type{cm->ret};
                    }
                    diags_.error(e->loc,
                        std::string("metodo desconocido '") + fa->field_name +
                        "' sobre tipo " + primitive_name(base_t.kind));
                    for (auto &a : e->args) (void)check_expr(a.get());
                    return Type{};
                }
            }
        }

        // ------------------------------------------------------------
        // Reflexion OO: dispatch para `cls.getMethod`, `m.invoke`, etc.
        // Se activa cuando el base es un IdentExpr resolviendo a un
        // Symbol con `reflection_alias` no vacio (declarado como
        // `Class cls`, `Method m`, `Field f`, `Object o`), o cuando el
        // base es el identifier literal "Class"/"Method"/"Field"
        // (forma estatica `Class.forName`).  Cada metodo se desazucara
        // a su builtin standalone equivalente (forName, getMethod, etc.)
        // sin cambios en el runtime: solo conveniencia sintactica.
        // ------------------------------------------------------------
        if (e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *idb = static_cast<ast::IdentExpr *>(fa->base.get());
                std::string alias_kind;  // "Class" | "Method" | "Field" | "Object" o vacio
                bool is_static_form = false;
                // Forma estatica: literal "Class"/"Method"/etc. como base.
                if (idb->name == "Class" || idb->name == "Method"
                 || idb->name == "Field" || idb->name == "Object") {
                    if (lookup(idb->name) == nullptr) {
                        // No es una variable real con ese nombre; es la
                        // forma estatica.  El base no se evaluara.
                        alias_kind = idb->name;
                        is_static_form = true;
                    }
                }
                // Forma instancia: variable cuyo Symbol tiene reflection_alias.
                if (!is_static_form) {
                    if (const Symbol *sym = lookup(idb->name)) {
                        if (!sym->reflection_alias.empty()) {
                            alias_kind = sym->reflection_alias;
                        }
                    }
                }
                if (!alias_kind.empty()) {
                    // Mapeo de metodo OO -> builtin standalone equivalente
                    // y validacion de aridad.  El lowering hace la reescritura
                    // efectiva (CallExpr al builtin con base prepended).
                    struct DispatchEntry {
                        const char *alias;
                        const char *method;
                        const char *builtin;
                        int min_args;
                        int max_args;  // -1 = variadico
                        PrimitiveKind ret;
                        bool needs_self;  // true: prepend base como primer arg
                    };
                    // Tabla declarativa.  args contados sin contar self.
                    static const DispatchEntry MAP[] = {
                        // Estatico: Class.forName(name)
                        {"Class", "forName",     "forName",     1, 1, PrimitiveKind::I64, false},
                        // Instancia: Class -> getMethod / getField / newInstance / getMethods
                        {"Class", "getMethod",   "getMethod",   1, 1, PrimitiveKind::I64, true},
                        {"Class", "getField",    "getField",    1, 1, PrimitiveKind::I64, true},
                        {"Class", "newInstance", "newInstance", 0, 0, PrimitiveKind::I64, true},
                        {"Class", "getMethods",  "getMethods",  0, 0, PrimitiveKind::I64, true},
                        // Instancia: Method -> invoke
                        {"Method", "invoke",     "invoke",      1, -1, PrimitiveKind::I64, true},
                        // Instancia: Object -> getClass (alias del builtin)
                        {"Object", "getClass",   "getClass",    0, 0, PrimitiveKind::I64, true},
                    };
                    const DispatchEntry *match = nullptr;
                    for (const auto &e0 : MAP) {
                        if (alias_kind == e0.alias && fa->field_name == e0.method) {
                            // Filtrar por static/instance segun forma.
                            if (is_static_form && !e0.needs_self) { match = &e0; break; }
                            if (!is_static_form && e0.needs_self) { match = &e0; break; }
                        }
                    }
                    if (match) {
                        const int n = static_cast<int>(e->args.size());
                        if (n < match->min_args
                         || (match->max_args >= 0 && n > match->max_args)) {
                            diags_.error(e->loc,
                                std::string(alias_kind) + "." + match->method +
                                ": numero de argumentos incorrecto (recibidos " +
                                std::to_string(n) + ")");
                        }
                        // Validacion de tipos minima: solo evaluamos los args
                        // para que tengan result_type asignado (el lowering
                        // hara la coercion final).
                        for (auto &a : e->args) (void)check_expr(a.get());
                        // Marca al callee con dispatch_kind para que el
                        // lowering lo reconozca y emita la builtin.
                        // Usamos property_kind como vehiculo (ya existe en
                        // FieldAccessExpr para getter/setter de propiedad).
                        // Codigos:
                        //   100 = forName        (estatico)
                        //   101 = getMethod
                        //   102 = getField
                        //   103 = newInstance
                        //   104 = getMethods
                        //   105 = invoke
                        //   106 = getClass
                        if      (match->method == std::string("forName"))     fa->property_kind = 100;
                        else if (match->method == std::string("getMethod"))   fa->property_kind = 101;
                        else if (match->method == std::string("getField"))    fa->property_kind = 102;
                        else if (match->method == std::string("newInstance")) fa->property_kind = 103;
                        else if (match->method == std::string("getMethods"))  fa->property_kind = 104;
                        else if (match->method == std::string("invoke"))      fa->property_kind = 105;
                        else if (match->method == std::string("getClass"))    fa->property_kind = 106;
                        const Type rt{match->ret};
                        e->result_type = rt;
                        return rt;
                    }
                }
            }
        }

        // -------------------------------------------------------------
        // ADTs: si el callee es un FieldAccessExpr con base que es
        // un IDENTIFIER nombrando un enum (e.g. `Color.Red(42)`), lo
        // tratamos como CONSTRUCTOR de variante en lugar de llamada a
        // metodo.  Validamos:
        //   - El identifier de la izquierda nombra un enum registrado.
        //   - El identifier de la derecha nombra una variante existente.
        //   - El numero de argumentos coincide con el payload de la
        //     variante.
        // El tipo resultado es @c Type{STRUCT, "Color"} (mismo trick
        // que para enums "estaticos": reusamos kind=STRUCT con el
        // struct_name del enum).
        // -------------------------------------------------------------
        if (e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            // M.L7 ext: enum cross-module via namespace qualified.
            // `command.Command.InsertChar(65)` -- aqui `fa->base` es
            // FieldAccessExpr ("command.Command"), no IdentExpr.  Si su
            // result_type es STRUCT cuyo nombre coincide con un enum
            // registrado, lo tratamos como constructor de variante.
            if (fa->base
             && fa->base->kind == ast::NodeKind::FieldAccessExpr) {
                // Forzar el check del base para que poblee result_type.
                Type bt = check_expr(fa->base.get());
                if (bt.kind == PrimitiveKind::STRUCT
                 && !bt.struct_name.empty()) {
                    auto it_en = enum_layouts_.find(bt.struct_name);
                    if (it_en != enum_layouts_.end()) {
                        const EnumLayout &elay = it_en->second;
                        const EnumVariantInfo *var = nullptr;
                        for (const auto &v : elay.variants) {
                            if (v.name == fa->field_name) { var = &v; break; }
                        }
                        if (!var) {
                            diags_.error(e->loc,
                                "variante desconocida '" + fa->field_name +
                                "' en enum '" + elay.name + "'");
                            for (auto &a : e->args) (void)check_expr(a.get());
                            return Type{};
                        }
                        if (e->args.size() != var->field_types.size()) {
                            diags_.error(e->loc,
                                std::string("variante '") + var->name +
                                "': esperados " + std::to_string(var->field_types.size()) +
                                " argumentos, recibidos " + std::to_string(e->args.size()));
                        }
                        const size_t n = std::min(e->args.size(), var->field_types.size());
                        for (size_t i = 0; i < n; ++i) {
                            const Type ta = check_expr(e->args[i].get());
                            const Type &tp = var->field_types[i];
                            if (ta.kind == PrimitiveKind::COUNT) continue;
                            if (!types_assignable(tp, ta)) {
                                diags_.error(e->args[i]->loc,
                                    std::string("variante '") + var->name +
                                    "', payload " + std::to_string(i + 1) + ": tipo (" +
                                    type_to_string(ta) + ") incompatible con declarado (" +
                                    type_to_string(tp) + ")");
                            }
                        }
                        for (size_t i = n; i < e->args.size(); ++i)
                            (void)check_expr(e->args[i].get());
                        fa->property_kind = 99;
                        Type rt{PrimitiveKind::STRUCT, elay.name};
                        fa->result_type = rt;
                        return rt;
                    }
                }
            }
            if (fa->base
             && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *base_id = static_cast<ast::IdentExpr *>(fa->base.get());
                // L2.3: enum generico template `Maybe.Some(42)` -- inferir
                // monomorph desde args si es posible, o usar expected_enum
                // stack desde el contexto.
                if (is_generic_enum_template(base_id->name)) {
                    // Localizar template AST + variante.
                    auto it_tmpl = generic_enum_templates_.find(base_id->name);
                    const ast::EnumDecl *tmpl =
                        (it_tmpl != generic_enum_templates_.end())
                            ? static_cast<const ast::EnumDecl *>(
                                mod_.decls[it_tmpl->second].get())
                            : nullptr;
                    const ast::EnumVariantDecl *tvar = nullptr;
                    if (tmpl) {
                        for (const auto &v : tmpl->variants) {
                            if (v.name == fa->field_name) { tvar = &v; break; }
                        }
                    }
                    // Inferir args concretos por type_param: para cada
                    // type_param T, buscar la primera variante payload que
                    // usa T como NamedTypeNode y leer el arg correspondiente
                    // del call (su tipo concreto).
                    std::vector<Type> infer_args(tmpl ? tmpl->type_params.size() : 0,
                                                  Type{});
                    bool fully_inferred = (tmpl && !tmpl->type_params.empty());
                    if (tmpl && tvar) {
                        const size_t n = std::min(e->args.size(), tvar->field_types.size());
                        std::vector<Type> arg_types(n);
                        for (size_t i = 0; i < n; ++i)
                            arg_types[i] = check_expr(e->args[i].get());
                        for (size_t i = n; i < e->args.size(); ++i)
                            (void)check_expr(e->args[i].get());
                        for (size_t tpi = 0; tpi < tmpl->type_params.size(); ++tpi) {
                            const std::string &tp_name = tmpl->type_params[tpi];
                            for (size_t pi = 0; pi < tvar->field_types.size() && pi < n; ++pi) {
                                const ast::TypeNode *ft = tvar->field_types[pi].get();
                                if (ft && ft->kind == ast::NodeKind::NamedTypeNode) {
                                    const auto *nt = static_cast<const ast::NamedTypeNode *>(ft);
                                    if (nt->name == tp_name) {
                                        infer_args[tpi] = arg_types[pi];
                                        break;
                                    }
                                }
                            }
                            if (infer_args[tpi].kind == PrimitiveKind::COUNT
                             || infer_args[tpi].kind == PrimitiveKind::VOID) {
                                fully_inferred = false;
                            }
                        }
                    } else {
                        for (auto &a : e->args) (void)check_expr(a.get());
                    }

                    std::string mangled;
                    // Prefer expected (LHS context) sobre inferencia desde
                    // args, porque literales como `42` defaultean a i64
                    // pero el LHS puede pedir i32.
                    if (const std::string *expected =
                            expected_enum_mangled(base_id->name)) {
                        mangled = *expected;
                    } else if (fully_inferred) {
                        mangled = monomorphize_enum(base_id->name, infer_args, e->loc);
                    }
                    if (mangled.empty()) {
                        diags_.error(e->loc,
                            "no se puede inferir args de tipo para enum generico '" +
                            base_id->name + "'");
                        return Type{};
                    }
                    auto it_mono = enum_layouts_.find(mangled);
                    if (it_mono == enum_layouts_.end()) {
                        // Trigger monomorphize y pase 2 re-run no es viable aqui;
                        // forzar el layout via run_pass2_for_decl no esta expuesto.
                        // Como fallback, reportar y salir.
                        diags_.error(e->loc,
                            "enum generico '" + mangled + "' no esta registrado");
                        return Type{};
                    }
                    const EnumLayout &elay = it_mono->second;
                    const EnumVariantInfo *var = nullptr;
                    for (const auto &v : elay.variants) {
                        if (v.name == fa->field_name) { var = &v; break; }
                    }
                    if (!var) {
                        diags_.error(e->loc,
                            "variante desconocida '" + fa->field_name +
                            "' en enum '" + elay.name + "'");
                        return Type{};
                    }
                    fa->property_kind = 99;
                    Type rt{PrimitiveKind::STRUCT, elay.name};
                    fa->result_type = rt;
                    // Reescribir el base_id al mangled para que el lowering
                    // dispatche al enum concreto.
                    base_id->name = elay.name;
                    return rt;
                }
                auto it_en = enum_layouts_.find(base_id->name);
                if (it_en != enum_layouts_.end()) {
                    const EnumLayout &elay = it_en->second;
                    const EnumVariantInfo *var = nullptr;
                    for (const auto &v : elay.variants) {
                        if (v.name == fa->field_name) { var = &v; break; }
                    }
                    if (!var) {
                        diags_.error(e->loc,
                            "variante desconocida '" + fa->field_name +
                            "' en enum '" + elay.name + "'");
                        for (auto &a : e->args) (void)check_expr(a.get());
                        return Type{};
                    }
                    // Aridad y tipos de argumentos.
                    if (e->args.size() != var->field_types.size()) {
                        diags_.error(e->loc,
                            std::string("variante '") + var->name +
                            "': esperados " + std::to_string(var->field_types.size()) +
                            " argumentos, recibidos " + std::to_string(e->args.size()));
                    }
                    const size_t n = std::min(e->args.size(), var->field_types.size());
                    for (size_t i = 0; i < n; ++i) {
                        const Type ta = check_expr(e->args[i].get());
                        const Type &tp = var->field_types[i];
                        if (ta.kind == PrimitiveKind::COUNT) continue;
                        if (!types_assignable(tp, ta)) {
                            diags_.error(e->args[i]->loc,
                                std::string("variante '") + var->name +
                                "', payload " + std::to_string(i + 1) + ": tipo (" +
                                type_to_string(ta) + ") incompatible con declarado (" +
                                type_to_string(tp) + ")");
                        }
                    }
                    for (size_t i = n; i < e->args.size(); ++i) (void)check_expr(e->args[i].get());
                    // Marcar el FieldAccessExpr y CallExpr para que el
                    // lowering los reconozca como constructor de variante.
                    // Usamos `property_kind` como flag: 99 indica "es
                    // constructor de variante de enum"; el lowering hace
                    // un dispatch dedicado al ver este valor.
                    fa->property_kind = 99;
                    Type rt{PrimitiveKind::STRUCT, elay.name};
                    fa->result_type = rt;
                    return rt;
                }
            }
        }

        // Caso A: obj.method(args) - callee es FieldAccessExpr.  El base
        // debe ser de tipo CLASS (instancia con vtable).  Los structs son
        // value types y sus metodos se desugaran a funciones libres
        // tomando @c Struct* como primer argumento, asi que el dispatch
        // de @c struct_instance.method() pasa por la ruta de free function
        // call, no por aqui.
        if (e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            const Type bt = check_expr(fa->base.get());
            if (bt.kind != PrimitiveKind::CLASS) {
                diags_.error(e->loc,
                    "invocacion de metodo sobre tipo no-clase: " + type_to_string(bt));
                for (auto &a : e->args) (void)check_expr(a.get());
                return Type{};
            }
            auto it = class_layouts_.find(bt.struct_name);
            if (it == class_layouts_.end()) {
                diags_.error(e->loc, "clase desconocida: '" + bt.struct_name + "'");
                for (auto &a : e->args) (void)check_expr(a.get());
                return Type{};
            }
            const ClassLayout &cls = it->second;
            const ClassMethodInfo *mtd = nullptr;
            for (const auto &m : cls.methods) {
                if (m.is_constructor) continue;
                if (m.name == fa->field_name) { mtd = &m; break; }
            }
            if (!mtd) {
                diags_.error(e->loc,
                    "la clase '" + bt.struct_name + "' no tiene un metodo '" +
                    fa->field_name + "'");
                for (auto &a : e->args) (void)check_expr(a.get());
                return Type{};
            }
            // Enforcement de visibilidad en metodos (private = solo dentro
            // de la misma clase).  Buscamos el ClassMethodDecl original
            // en el AST para consultar el flag access.
            for (auto &d : mod_.decls) {
                if (!d || d->kind != ast::NodeKind::ClassDecl) continue;
                auto *cdp = static_cast<const ast::ClassDecl *>(d.get());
                if (cdp->name != bt.struct_name) continue;
                for (auto &mm : cdp->methods) {
                    if (mm && !mm->is_constructor && mm->name == fa->field_name) {
                        if (mm->access == 1 /*private*/
                         && current_class_ != bt.struct_name) {
                            diags_.error(e->loc,
                                "metodo privado '" + fa->field_name +
                                "' de la clase '" + bt.struct_name +
                                "' no es accesible desde fuera de la clase");
                        }
                        break;
                    }
                }
                break;
            }
            // Aridad y tipos de arg.
            if (e->args.size() != mtd->param_types.size()) {
                diags_.error(e->loc,
                    "numero de argumentos incorrecto en metodo '" + fa->field_name +
                    "': esperados " + std::to_string(mtd->param_types.size()) +
                    ", recibidos " + std::to_string(e->args.size()));
            }
            const size_t n = std::min(e->args.size(), mtd->param_types.size());
            for (size_t i = 0; i < n; ++i) {
                const Type ta = check_expr(e->args[i].get());
                const Type &tp = mtd->param_types[i];
                if (ta.kind == PrimitiveKind::COUNT) continue;
                if (!types_assignable(tp, ta)) {
                    diags_.error(e->args[i]->loc,
                        std::string("argumento ") + std::to_string(i + 1) +
                        " del metodo '" + fa->field_name + "': tipo (" +
                        type_to_string(ta) + ") incompatible con parametro (" +
                        type_to_string(tp) + ")");
                }
            }
            for (size_t i = n; i < e->args.size(); ++i) (void)check_expr(e->args[i].get());
            // Anotar el tipo del FieldAccessExpr como el tipo de retorno
            // (util para que el lowering tenga el target_type listo).
            fa->result_type = mtd->return_type;
            return mtd->return_type;
        }

        // Caso B: llamada normal a funcion top-level.
        if (e->callee->kind != ast::NodeKind::IdentExpr) {
            diags_.error(e->loc, "se esperaba un nombre de funcion como callee");
            for (auto &a : e->args) (void)check_expr(a.get());
            return Type{};
        }
        /* A.43.13: aliases globales para builtins comptime.  Reduce
         * verbosidad permitiendo escribir `concat(a, b)` en vez de
         * `comptime_concat(a, b)`, `replace(s, n, v)` en vez de
         * `comptime_replace(...)`, etc.  Los aliases mutan el AST
         * in-place (renombran el IdentExpr a su nombre canonico) asi
         * todas las branches downstream que comparan @c id->name siguen
         * funcionando sin cambios.  Solo se aplica a CallExpr; no
         * afecta a IdentExpr en otras posiciones.
         *
         * Nota: NO aliasamos `print` (colisiona con runtime print) ni
         * nombres ya usados por la stdlib.  El usuario que quiera ser
         * explicito puede seguir escribiendo `comptime_*`. */
        {
            auto *id_mut = static_cast<ast::IdentExpr *>(e->callee.get());
            static const std::pair<const char *, const char *> ALIASES[] = {
                {"concat",      "comptime_concat"},
                {"streq",       "comptime_streq"},
                {"strlen",      "comptime_strlen"},
                {"chr",         "comptime_chr"},
                {"ord",         "comptime_ord"},
                {"substr",      "comptime_substr"},
                {"repeat",      "comptime_repeat"},
                {"to_str",      "comptime_to_str"},
                {"replace",     "comptime_replace"},
                {"contains",    "comptime_contains"},
                {"emit_expr",   "comptime_emit_expr"},
                {"compile",     "comptime_compile"},
                {"ct_print",    "comptime_print"},
            };
            for (const auto &a : ALIASES) {
                if (id_mut->name == a.first) {
                    id_mut->name = a.second;
                    break;
                }
            }
        }
        const auto *id = static_cast<const ast::IdentExpr *>(e->callee.get());

        // -----------------------------------------------------------------
        // Builtins comptime de introspection (Sprint 1).
        // Toman <T> en e->type_args.  Resolvidos a CONSTANTES literales
        // por el lowering.  Cero overhead runtime.  Validamos:
        //   1. e->type_args.size() == aridad esperada.
        //   2. e->args.size() == 0 (los del sprint 1 son nullary).
        //   3. T es resoluble (type_from_node devuelve algo distinto a VOID).
        // El RETURN type lo fijamos aqui; el VALOR concreto lo computa
        // lowering invocando los helpers en comptime_introspect.h.
        // -----------------------------------------------------------------
        if (e->type_args.size() >= 1
         && (id->name == "sizeof"   || id->name == "alignof"
          || id->name == "typename" || id->name == "type_id"
          || id->name == "kind"     || id->name == "comptime_type"
          || id->name == "parent_class" || id->name == "element_type"
          || id->name == "error_type")) {
            if (e->type_args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 type arg <T>, recibidos "
                    + std::to_string(e->type_args.size()));
            }
            if (!e->args.empty()) {
                diags_.error(e->loc,
                    id->name + ": no acepta argumentos runtime (solo <T>)");
            }
            /* Validar que T sea resoluble.  type_from_node devuelve un
             * Type con kind=VOID si no logro resolver. */
            Type resolved = e->type_args.empty()
                ? Type{}
                : type_from_node(e->type_args[0].get());
            if (resolved.kind == PrimitiveKind::VOID
             && e->type_args.size() == 1) {
                /* Si el usuario escribio sizeof<void>() o similar,
                 * permitirlo (sizeof(void)=0).  Pero si fue por fallo de
                 * resolucion, type_from_node ya emitio diagnostico. */
            }
            /* Tipo de retorno segun el builtin. */
            Type rt{};
            if (id->name == "sizeof" || id->name == "alignof") {
                rt = Type{PrimitiveKind::U64};
            } else if (id->name == "typename") {
                rt = Type{PrimitiveKind::STRING};
            } else if (id->name == "type_id") {
                rt = Type{PrimitiveKind::U32};
            } else if (id->name == "kind") {
                rt = Type{PrimitiveKind::I32};
            } else if (id->name == "comptime_type"
                    || id->name == "parent_class"
                    || id->name == "element_type"
                    || id->name == "error_type") {
                /* devuelven un Type como first-class value.  Solo
                 * usable como init de `comptime const Type X = ...`. */
                rt = Type{PrimitiveKind::TYPE_META};
            }
            e->result_type = rt;
            return rt;
        }

        // -----------------------------------------------------------------
        // introspection: acceso directo a campos via offset
        // compile-time.  Bypass de getfield/setfield -- el offset se
        // resuelve via comptime_field_offset y el LOAD/STORE va directo.
        //
        //   field_get<T>(obj: T, "f")        -> typeof(T.f)
        //   field_set<T>(obj: T, "f", value) -> void
        //
        // Ventajas vs `obj.f` plano:
        //   - bypass del CALLVIRT a getter de propiedad
        //   - bypass del RMW de bit fields (acceso al storage word directo)
        //   - acceso "raw" util para serializadores / hashers genericos
        // -----------------------------------------------------------------
        if (!e->type_args.empty()
         && (id->name == "field_get" || id->name == "field_set")) {
            const bool is_get = (id->name == "field_get");
            if (e->type_args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 type arg <T>, recibidos "
                    + std::to_string(e->type_args.size()));
            }
            const size_t expected_args = is_get ? 2 : 3;
            if (e->args.size() != expected_args) {
                diags_.error(e->loc,
                    id->name + ": se esperaban " + std::to_string(expected_args)
                    + " argumentos (obj, \"field\""
                    + (is_get ? "" : ", value") + "), recibidos "
                    + std::to_string(e->args.size()));
            }
            const Type t = e->type_args.empty()
                ? Type{} : type_from_node(e->type_args[0].get());
            /* obj: debe ser STRUCT/CLASS compatible con T. */
            if (!e->args.empty()) {
                const Type ot = check_expr(e->args[0].get());
                if (ot.kind != PrimitiveKind::CLASS
                 && ot.kind != PrimitiveKind::STRUCT
                 && ot.kind != PrimitiveKind::COUNT) {
                    diags_.error(e->args[0]->loc,
                        id->name + ": el primer argumento debe ser una "
                        "instancia de tipo " + type_to_string(t)
                        + ", recibido '" + type_to_string(ot) + "'");
                }
            }
            /* segundo arg: string literal compile-time con el nombre. */
            std::string fname;
            if (e->args.size() >= 2) {
                auto *slit = dynamic_cast<ast::StringLitExpr *>(
                    e->args[1].get());
                if (!slit || slit->is_interpolated()) {
                    diags_.error(e->args[1]->loc,
                        id->name + ": el segundo argumento debe ser un "
                        "literal string compile-time (no interpolado)");
                } else {
                    fname = slit->value;
                }
                (void)check_expr(e->args[1].get());
            }
            /* Resolver el tipo del campo en T. */
            Type ftype = comptime_field_type(*this, t, fname);
            if (ftype.kind == PrimitiveKind::COUNT && !fname.empty()) {
                diags_.error(e->loc,
                    id->name + ": el tipo '" + type_to_string(t)
                    + "' no tiene campo '" + fname + "'");
            }
            /* field_set: tercer arg debe ser asignable al tipo del campo. */
            if (!is_get && e->args.size() >= 3) {
                const Type vt = check_expr(e->args[2].get());
                if (ftype.kind != PrimitiveKind::COUNT
                 && !types_assignable(ftype, vt)) {
                    diags_.error(e->args[2]->loc,
                        "field_set: el valor de tipo '" + type_to_string(vt)
                        + "' no es asignable al campo '" + fname
                        + "' de tipo '" + type_to_string(ftype) + "'");
                }
            }
            /* Tipo de retorno. */
            const Type rt = is_get
                ? (ftype.kind == PrimitiveKind::COUNT ? Type{} : ftype)
                : Type{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }

        // -----------------------------------------------------------------
        // Sprint 3-C introspection: for_each_field<T>(cb) / for_each_method.
        // El callback se invoca UNA vez por cada field/method de T en
        // compile-time (loop completamente unrolled).  La firma del
        // callback debe ser `fn(string) -> void` (o `fn(string) -> T`
        // ignorando el retorno).
        // -----------------------------------------------------------------
        if (!e->type_args.empty()
         && (id->name == "for_each_field" || id->name == "for_each_method")) {
            if (e->type_args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 type arg <T>, recibidos "
                    + std::to_string(e->type_args.size()));
            }
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 argumento (callback), recibidos "
                    + std::to_string(e->args.size()));
            }
            (void)type_from_node(e->type_args[0].get());
            if (!e->args.empty()) {
                const Type cbt = check_expr(e->args[0].get());
                /* El callback debe ser FUNCTION tomando 1 string. */
                if (cbt.kind != PrimitiveKind::FUNCTION
                 || cbt.fn_params.size() != 1
                 || cbt.fn_params[0].kind != PrimitiveKind::STRING) {
                    diags_.error(e->args[0]->loc,
                        id->name + ": el callback debe tener firma "
                        "fn(string) -> _ (recibe el nombre del field/method)");
                }
            }
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }

        // -----------------------------------------------------------------
        // Sprint 2 introspection: queries de fields/methods + queries de tipos.
        // Aridades:
        //   1 type_arg, 0 runtime args:  field_count, method_count,
        //                                 is_class, is_struct, is_primitive
        //   1 type_arg, 1 runtime arg:   offsetof, has_field, has_method,
        //                                 field_type (arg debe ser string lit)
        //                                 field_name (arg debe ser int lit)
        //   2 type_args, 0 runtime args: is_subtype, is_same
        // -----------------------------------------------------------------
        {
            const std::string &nm = id->name;
            const bool one_targ_no_args =
                nm == "field_count"  || nm == "method_count"
             || nm == "is_class"     || nm == "is_struct"
             || nm == "is_primitive"
             || nm == "is_newtype"   || nm == "is_opaque"
             || nm == "underlying_of";
            const bool one_targ_str_arg =
                nm == "offsetof"     || nm == "has_field"
             || nm == "has_method"   || nm == "field_type";
            const bool one_targ_int_arg = (nm == "field_name"
                                         || nm == "field_type_at"
                                         || nm == "method_name"
                                         || nm == "method_return_type");
            const bool two_targ_no_args =
                nm == "is_subtype"   || nm == "is_same";

            if ((one_targ_no_args || one_targ_str_arg
              || one_targ_int_arg || two_targ_no_args)
             && !e->type_args.empty()) {
                /* Aridad de type_args. */
                const size_t expected_targs = two_targ_no_args ? 2 : 1;
                if (e->type_args.size() != expected_targs) {
                    diags_.error(e->loc,
                        nm + ": se esperaban " + std::to_string(expected_targs)
                        + " type args, recibidos "
                        + std::to_string(e->type_args.size()));
                }
                /* Aridad de runtime args. */
                const size_t expected_args =
                    (one_targ_str_arg || one_targ_int_arg) ? 1 : 0;
                if (e->args.size() != expected_args) {
                    diags_.error(e->loc,
                        nm + ": se esperaban " + std::to_string(expected_args)
                        + " argumentos runtime, recibidos "
                        + std::to_string(e->args.size()));
                }
                /* Validar que los type_args resuelvan. */
                for (auto &ta : e->type_args) (void)type_from_node(ta.get());
                /* Arg literal compile-time (string lit no interpolado, o
                 * int lit).  El lowering lo asume garantizado. */
                if (one_targ_str_arg && !e->args.empty()) {
                    auto *slit = dynamic_cast<ast::StringLitExpr *>(
                        e->args[0].get());
                    if (!slit || slit->is_interpolated()) {
                        diags_.error(e->args[0]->loc,
                            nm + ": el argumento debe ser un literal string "
                            "compile-time (no interpolado, no variable)");
                    }
                    (void)check_expr(e->args[0].get());
                }
                if (one_targ_int_arg && !e->args.empty()) {
                    auto *ilit = dynamic_cast<ast::IntLitExpr *>(
                        e->args[0].get());
                    if (!ilit) {
                        diags_.error(e->args[0]->loc,
                            nm + ": el argumento debe ser un literal entero "
                            "compile-time (no variable, no expresion)");
                    }
                    (void)check_expr(e->args[0].get());
                }
                /* Tipo de retorno segun el builtin. */
                Type rt{};
                if (nm == "offsetof") {
                    rt = Type{PrimitiveKind::U64};
                } else if (nm == "field_count" || nm == "method_count") {
                    rt = Type{PrimitiveKind::U32};
                } else if (nm == "field_name" || nm == "field_type"
                         || nm == "underlying_of") {
                    rt = Type{PrimitiveKind::STRING};
                } else if (nm == "field_type_at") {
                    /* A.43: field_type_at<T>(idx) -> Type as first-class value. */
                    rt = Type{PrimitiveKind::TYPE_META};
                } else if (nm == "method_name") {
                    /* A.43: method_name<T>(idx) -> string. */
                    rt = Type{PrimitiveKind::STRING};
                } else if (nm == "method_return_type") {
                    /* A.43: method_return_type<T>(idx) -> Type. */
                    rt = Type{PrimitiveKind::TYPE_META};
                } else {
                    /* has_field/has_method/is_subtype/is_same/is_class/
                     * is_struct/is_primitive -> BOOL. */
                    rt = Type{PrimitiveKind::BOOL};
                }
                e->result_type = rt;
                return rt;
            }
        }

        // -----------------------------------------------------------------
        // A.39: builtins comptime sobre strings.
        //   comptime_concat(a, b) -> string  (concat de 2 strings comptime)
        //   comptime_streq(a, b)  -> bool    (igualdad de strings comptime)
        //   comptime_strlen(s)    -> u64     (longitud en bytes)
        //
        // Estos son SIEMPRE compile-time -- args deben ser comptime-
        // evaluables a string.  El lowering los inlinea como literal
        // (string -> STR_LIT_ADDR+STRMAKE; int -> CONST) sin runtime call.
        // -----------------------------------------------------------------
        /* A.43.12: comptime_replace(s, needle, replacement) y
         * comptime_contains(s, needle) -- patron declarativo de templates
         * para macros.  Combinados con `comptime_emit_expr` permiten
         * generar codigo a partir de templates con placeholders. */
        if (id->name == "comptime_replace") {
            if (e->args.size() != 3) {
                diags_.error(e->loc,
                    "comptime_replace: se esperaban 3 args (str, needle, replacement), recibidos "
                    + std::to_string(e->args.size()));
            } else {
                for (auto &a : e->args) (void)check_expr(a.get());
            }
            Type rt{PrimitiveKind::STRING};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "comptime_contains") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "comptime_contains: se esperaban 2 args (str, needle), recibidos "
                    + std::to_string(e->args.size()));
            } else {
                for (auto &a : e->args) (void)check_expr(a.get());
            }
            Type rt{PrimitiveKind::BOOL};
            e->result_type = rt;
            return rt;
        }
        /* A.43.11: gensym(prefix) -> string.  Devuelve un identifier
         * fresco unico, util para macros hygenic (prevenir capture en
         * el scope del caller).  Validacion minima: 1 arg string. */
        if (id->name == "gensym") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "gensym: se esperaba 1 argumento (string prefix), recibidos "
                    + std::to_string(e->args.size()));
            } else {
                (void)check_expr(e->args[0].get());
            }
            Type rt{PrimitiveKind::STRING};
            e->result_type = rt;
            return rt;
        }
        /* comptime_emit_expr(str) -- macros Lisp con splice/emit.
         * El string se parsea como una EXPRESION Vex, se type-checa en el
         * contexto actual y se SUSTITUYE en el AST runtime (no solo
         * comptime eval).  El lowering ve el AST sustituido y emite
         * codigo runtime real.  Equivalente al unquote/splice de Lisp.
         * Diferencia con comptime_compile: este SI emite codigo runtime;
         * comptime_compile solo evalua al compile-time y descarta el AST. */
        if (id->name == "comptime_emit_expr") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "comptime_emit_expr: se esperaba 1 argumento (string), recibidos "
                    + std::to_string(e->args.size()));
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            const ComptimeEvalResult sarg = comptime_eval_expr(*this, e->args[0].get());
            if (!sarg.ok || !sarg.is_str) {
                diags_.error(e->args[0]->loc,
                    "comptime_emit_expr: el argumento debe ser un string comptime-evaluable");
                return Type{};
            }
            /* Parsear el fragmento como expresion Vex. */
            Lexer fragment_lex(sarg.str, "<comptime_emit_expr>", diags_);
            Parser fragment_par(fragment_lex, diags_);
            std::unique_ptr<ast::Expr> parsed = fragment_par.parse_one_expr();
            if (!parsed) {
                diags_.error(e->loc,
                    "comptime_emit_expr: el fragmento no se pudo parsear como expresion");
                return Type{};
            }
            parsed->loc = e->loc;
            /* Type-checar la expresion sustituida en el contexto actual. */
            const Type rt = check_expr(parsed.get());
            /* Guardar el AST sustituido para que el lowering lo recoja. */
            e->macro_expanded = std::move(parsed);
            e->result_type    = rt;
            return rt;
        }
        /* comptime_compile(str) -> result.  MVP de macros estilo
         * Lisp: el string se parsea como una EXPRESION Vex y se evalua
         * en compile-time.  Permite construir codigo a partir de datos
         * (typename<T>, comptime_concat, comptime_to_str, etc.) y
         * ejecutarlo sin runtime.  Limitaciones:
         *  - Solo expresion (no statements).
         *  - El tipo de retorno depende del contenido y se inferira
         *    desde el comptime const que reciba el valor; aqui en check
         *    devolvemos el tipo declarado del binding o un sentinela u64. */
        if (id->name == "comptime_compile") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "comptime_compile: se esperaba 1 argumento (string), recibidos "
                    + std::to_string(e->args.size()));
            }
            if (!e->args.empty()) {
                (void)check_expr(e->args[0].get());
                /* Eval temprano para reportar errores de parsing aqui. */
                const ComptimeEvalResult r = comptime_eval_expr(*this, e);
                if (!r.ok) {
                    diags_.error(e->loc,
                        "comptime_compile: el fragmento no es comptime-evaluable");
                }
                /* Devolver el tipo segun el contenido inferido. */
                Type rt{};
                if (r.is_str)        rt = Type{PrimitiveKind::STRING};
                else if (r.is_type)  rt = Type{PrimitiveKind::TYPE_META};
                else                 rt = Type{PrimitiveKind::I64};
                e->result_type = rt;
                return rt;
            }
            Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        /* comptime_print(value) -> u64 (=0).  Emite a stderr en
         * compile-time.  Acepta string/int/Type.  Validacion minima: 1 arg
         * comptime-evaluable.  Retorna u64=0 para componer en static_assert. */
        if (id->name == "comptime_print") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "comptime_print: se esperaba 1 argumento, recibidos "
                    + std::to_string(e->args.size()));
            }
            if (!e->args.empty()) {
                (void)check_expr(e->args[0].get());
                const ComptimeEvalResult r = comptime_eval_expr(*this, e->args[0].get());
                if (!r.ok) {
                    diags_.error(e->args[0]->loc,
                        "comptime_print: el argumento no es comptime-evaluable");
                }
            }
            Type rt{PrimitiveKind::U64};
            e->result_type = rt;
            return rt;
        }

        if (id->name == "comptime_concat"
         || id->name == "comptime_streq"
         || id->name == "comptime_strlen"
         || id->name == "comptime_chr"
         || id->name == "comptime_ord"
         || id->name == "comptime_substr"
         || id->name == "comptime_repeat"
         || id->name == "comptime_to_str") {
            const std::string &nm  = id->name;
            size_t expected;
            if (nm == "comptime_strlen" || nm == "comptime_chr"
             || nm == "comptime_ord"    || nm == "comptime_to_str") {
                expected = 1;
            } else if (nm == "comptime_substr") {
                expected = 3;
            } else {
                expected = 2;
            }
            if (e->args.size() != expected) {
                diags_.error(e->loc,
                    nm + ": se esperaban " + std::to_string(expected)
                    + " argumentos, recibidos "
                    + std::to_string(e->args.size()));
            }
            /* Chequear que los args sean comptime-evaluables.
             * El tipo (string vs int) depende del builtin:
             *   chr/repeat: arg int (codepoint o count)
             *   to_str: arg int
             *   ord: arg string
             *   substr: arg0 string, arg1/arg2 ints
             *   concat/streq/strlen: arg(s) string */
            for (auto &a : e->args) (void)check_expr(a.get());
            for (size_t i = 0; i < e->args.size(); ++i) {
                const ComptimeEvalResult r = comptime_eval_expr(*this, e->args[i].get());
                bool need_str = false;
                if (nm == "comptime_concat" || nm == "comptime_streq"
                 || nm == "comptime_strlen" || nm == "comptime_ord") {
                    need_str = true;
                } else if ((nm == "comptime_substr" || nm == "comptime_repeat")
                        && i == 0) {
                    need_str = true;
                }
                if (!r.ok) {
                    /* si el arg es un IdentExpr que resuelve
                     * a un `comptime var` (mutable) o `comptime const`
                     * en cualquier scope, NO emitimos error.  Esto
                     * cubre el caso de comptime_strlen(s) dentro de un
                     * @Macro body donde `s` esta declarada como
                     * `comptime var string s = "";` -- en type-check
                     * time s tiene valor "" (inicial) pero el call
                     * SITE del macro la evaluara con el valor mutado
                     * tras el loop.  El AST evaluator en
                     * comptime_introspect.cpp ya resuelve correctamente
                     * al call site.  Aqui solo el chequeo estatico es
                     * demasiado estricto. */
                    bool deferrable = false;
                    if (e->args[i]->kind == ast::NodeKind::IdentExpr) {
                        const auto *id = static_cast<const ast::IdentExpr *>(
                            e->args[i].get());
                        /* Buscar en local stack del lowering (comptime
                         * for index / comptime var locales). */
                        for (auto it = comptime_const_locals_.rbegin();
                             it != comptime_const_locals_.rend(); ++it) {
                            if (it->find(id->name) != it->end()) {
                                deferrable = true;
                                break;
                            }
                        }
                        /* Buscar en globales comptime const. */
                        if (!deferrable && comptime_const_values_.count(id->name)) {
                            deferrable = true;
                        }
                    }
                    if (!deferrable) {
                        diags_.error(e->args[i]->loc,
                            nm + ": argumento " + std::to_string(i)
                            + " no es comptime-evaluable");
                    }
                } else if (need_str && !r.is_str) {
                    diags_.error(e->args[i]->loc,
                        nm + ": argumento " + std::to_string(i)
                        + " debe ser string comptime");
                } else if (!need_str && r.is_str) {
                    diags_.error(e->args[i]->loc,
                        nm + ": argumento " + std::to_string(i)
                        + " debe ser int comptime");
                }
            }
            Type rt{};
            if (nm == "comptime_concat" || nm == "comptime_chr"
             || nm == "comptime_substr" || nm == "comptime_repeat"
             || nm == "comptime_to_str") {
                rt = Type{PrimitiveKind::STRING};
            } else if (nm == "comptime_streq") {
                rt = Type{PrimitiveKind::BOOL};
            } else {
                rt = Type{PrimitiveKind::U64};
            }
            e->result_type = rt;
            return rt;
        }

        // -----------------------------------------------------------------
        // static_assert(cond, "msg")
        // Verifica una condicion comptime-evaluable.  Si la cond es false
        // (o no evaluable), emite error de compile-time con el msg.  No
        // genera codigo runtime: el lowering devuelve IR_NO_VALUE (void).
        // -----------------------------------------------------------------
        if (id->name == "static_assert") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "static_assert: se esperaban 2 argumentos "
                    "(cond, \"msg\"), recibidos "
                    + std::to_string(e->args.size()));
                return Type{PrimitiveKind::VOID};
            }
            /* Validar tipo de la cond y del msg.  No es bloqueante --
             * intentamos evaluar de todas formas. */
            for (auto &a : e->args) (void)check_expr(a.get());
            /* Extraer msg literal. */
            std::string msg;
            auto *slit = dynamic_cast<ast::StringLitExpr *>(
                e->args[1].get());
            if (!slit || slit->is_interpolated()) {
                diags_.error(e->args[1]->loc,
                    "static_assert: el segundo argumento debe ser un "
                    "literal string compile-time (no interpolado)");
            } else {
                msg = slit->value;
            }
            /* try comptime eval first.  Si la cond ES
             * comptime-evaluable: fire diagnostic si false (same as
             * before).  Si NO ES (e.g. depende de macro param), NO
             * emitimos error -- el lowering bajara a CALLN
             * "vesta_comptime:static_assert" que evalua en runtime VM
             * (que sigue siendo compile time porque la macro corre en
             * ComptimeRuntime). */
            const ComptimeEvalResult r = comptime_eval_expr(
                *this, e->args[0].get());
            if (r.ok && r.value == 0) {
                diags_.error(e->loc,
                    std::string("static_assert FAILED: ")
                    + (msg.empty() ? std::string("condicion falsa") : msg));
            }
            /* Si !r.ok, no emitimos error -- el lowering despachara
             * via FFI al virtual fn que hace el check en runtime VM. */

            /* el tipo de retorno cambia segun el contexto.
             * Si la cond es comptime-evaluable (caso comun a nivel
             * modulo), devolvemos VOID -- el call site no usa el
             * resultado.  Si NO es (macros con runtime cond), devolvemos
             * I64 porque el lowering emitira CALLN a un fn que devuelve
             * i64 status (0=ok, 1=fail) y algunos sitios podrian leer
             * el valor. */
            const Type rt = r.ok
                ? Type{PrimitiveKind::VOID}
                : Type{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }

        // -----------------------------------------------------------------
        // Sprint 4 (A.37.s4): builtins runtime de introspection.
        //   find_type(name: string)              -> i64 (ptr a IntrospectInfo
        //                                            o 0 si no existe)
        //   type_info_kind(p)                    -> i32
        //   type_info_size(p)                    -> u32
        //   type_info_align(p)                   -> u32
        //   type_info_field_count(p)             -> u32
        //   type_info_name(p)                    -> string
        //   type_info_field_name(p, idx)         -> string
        //   type_info_field_offset(p, idx)       -> u32
        //   type_info_field_size(p, idx)         -> u32
        // -----------------------------------------------------------------
        {
            const std::string &nm = id->name;
            const bool is_find    = (nm == "find_type");
            const bool is_simple  =
                nm == "type_info_kind"  || nm == "type_info_size"
             || nm == "type_info_align" || nm == "type_info_field_count";
            const bool is_str_q   = (nm == "type_info_name");
            const bool is_field_idx_str =
                (nm == "type_info_field_name");
            const bool is_field_idx_num =
                nm == "type_info_field_offset"
             || nm == "type_info_field_size";
            if (is_find || is_simple || is_str_q
             || is_field_idx_str || is_field_idx_num) {
                const size_t expected = is_find ? 1
                                       : is_simple ? 1
                                       : is_str_q  ? 1
                                       : 2;
                if (e->args.size() != expected) {
                    diags_.error(e->loc,
                        nm + ": se esperaban " + std::to_string(expected)
                        + " argumentos, recibidos "
                        + std::to_string(e->args.size()));
                }
                /* Validar tipo del primer arg (string para find_type / _name;
                 * i64/ptr para type_info_*).  No imponemos restriccion
                 * estricta: aceptamos COUNT (inferido) y dejamos al lowering
                 * confiar en que el handle es un i64. */
                for (auto &a : e->args) (void)check_expr(a.get());
                if (is_find && !e->args.empty()) {
                    const Type ta = e->args[0]->result_type;
                    if (ta.kind != PrimitiveKind::STRING
                     && ta.kind != PrimitiveKind::PTR
                     && ta.kind != PrimitiveKind::COUNT) {
                        diags_.error(e->args[0]->loc,
                            "find_type: el argumento debe ser un string");
                    }
                }
                Type rt{};
                if (is_find) rt = Type{PrimitiveKind::I64};
                else if (nm == "type_info_kind")  rt = Type{PrimitiveKind::I32};
                else if (nm == "type_info_name" || nm == "type_info_field_name") rt = Type{PrimitiveKind::STRING};
                else rt = Type{PrimitiveKind::U32};
                e->result_type = rt;
                return rt;
            }
        }

        // -----------------------------------------------------------------
        // Builtins de reflexion.  No se declaran como funciones
        // normales; el lowering los baja a secuencias de instrucciones
        // bytecode existentes (findclass, mov, etc).  El tipo de retorno
        // de los punteros opacos es i64 (ClassInfo*/FieldInfo*/MethodInfo*).
        //
        //   forName(string lit / char* / string)   -> i64 (ClassInfo*)
        //   getClass(class_instance)                -> i64 (ClassInfo*)
        // -----------------------------------------------------------------
        if (id->name == "forName") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "forName: se esperaba 1 argumento (nombre de clase), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "getClass") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "getClass: se esperaba 1 argumento (instancia), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) {
                const Type at = check_expr(a.get());
                if (at.kind != PrimitiveKind::CLASS
                 && at.kind != PrimitiveKind::COUNT) {
                    diags_.error(a->loc,
                        "getClass: el argumento debe ser una instancia de clase, no '"
                        + type_to_string(at) + "'");
                }
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // getField(cls, "field_name") -> i64 (FieldInfo*).  cls debe ser
        // un i64 (resultado de forName/getClass) y el nombre un string lit.
        if (id->name == "getField") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "getField: se esperaban 2 argumentos (cls, name), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // Reflexion extendida.
        //
        //   getMethod(cls, "name")     -> i64 (MethodInfo*)
        //   newInstance(cls)           -> i64 (host_ptr a la nueva instancia,
        //                                  marcado is_host_ptr=true en lowering)
        //   invoke(method, this, ...)  -> i64 (resultado del dispatch via
        //                                  CALLM/advice_chain)
        //
        // Todos devuelven i64 generico (cast por el usuario al tipo logico).
        // El argumento variadico de invoke (this + args) se valida solo en
        // aridad >= 2.
        if (id->name == "getMethod") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "getMethod: se esperaban 2 argumentos (cls, name), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "newInstance") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "newInstance: se esperaba 1 argumento (cls), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "invoke") {
            if (e->args.size() < 2) {
                diags_.error(e->loc,
                    "invoke: se esperan al menos 2 argumentos (method, this, ...args), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // proceed() -> i64.  Solo valido dentro de un advice @Around; el
        // type checker no fuerza el contexto (lo hace el runtime: si se
        // ejecuta fuera de un AROUND frame, dispara
        // THREAD_ILLEGAL_INSTRUCTION).  Acepta cualquier tipo de retorno
        // pero declaramos i64 generico para que pase types_assignable.
        if (id->name == "proceed") {
            if (!e->args.empty()) {
                diags_.error(e->loc,
                    "proceed: no acepta argumentos (re-invoca el target con la calling convention actual)");
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // Optional via referencias nullable + instrucciones VM
        // isnull/unwrap (sin clase generica de wrapper).  Modelo:
        // cualquier referencia (CLASS) puede ser null; null literal
        // representa "ausente".  Builtins:
        //   isPresent(x) -> i32  (1 si x != null, 0 si x == null)
        //                    Lowering: isnull r_tmp, r_x; xor r_tmp, 1
        //   unwrap(x)    -> mismo tipo de x (host_ptr); throw si null.
        //                    Lowering: unwrap r_dst, r_x  (bytecode 0x26).
        // Pensados primariamente para reference types (CLASS); para
        // value types primitivos no hay null asi que el checker lo rechaza.
        // builtins de monitor para uso dentro de synchronized.
        //   wait(obj)      -> void   (libera monitor + suspende)
        //   notify(obj)    -> void   (despierta un waiter)
        //   notifyAll(obj) -> void   (despierta todos los waiters)
        // El argumento debe ser CLASS (referencia GC).  El llamador es
        // responsable de invocarlas dentro de synchronized(obj) {...},
        // si no, el bytecode subyacente fallara silenciosamente
        // (monwait sobre objeto sin lock = no-op + suspension indefinida).
        // builtins de procesos / IPC.
        //   pid()              -> i64    (PID encoded del proceso actual)
        //   msgsend(pid, val)  -> i32    (1 si enviado, 0 si error)
        //   msgrecv()          -> i64    (valor recibido del propio mailbox)
        if (id->name == "pid") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "pid: no acepta argumentos");
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // argv del script: builtins args_count() y args_get(i).
        // El runtime guarda los args en VM::script_args (poblado desde
        // main.cpp con todo lo que viene tras `--run prog.velb`).  Los
        // opcodes bytecode getargc (0x6B) y getarg (0x6C) los consultan.
        if (id->name == "args_count") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "args_count: no acepta argumentos");
            }
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        // Builtins de terminal / VT100.  Sin args (clear/save/restore/show/hide/reset)
        // o con (row, col) para term_move.  Cada uno baja a una secuencia
        // de vio_print con escapes ANSI hardcodeados.
        if (id->name == "term_clear" || id->name == "term_clear_line"
         || id->name == "term_save_cursor" || id->name == "term_restore_cursor"
         || id->name == "term_hide_cursor" || id->name == "term_show_cursor"
         || id->name == "term_reset") {
            if (!e->args.empty()) {
                diags_.error(e->loc, id->name + ": no acepta argumentos");
            }
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "term_move") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "term_move: se esperan 2 argumentos (row, col), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "args_get") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "args_get: se espera 1 argumento (i32 indice), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::STRING};
            e->result_type = rt;
            return rt;
        }
        // unloadmodule(path_lit): descarga un modulo dinamico previamente
        // cargado via loadmodule.  Devuelve i32 (1 ok, 0 no encontrado).
        // path debe ser string literal por las mismas razones que loadmodule:
        // se interna en static_data en compile time.
        // Reflexion: enumeracion de miembros de una clase.
        //   getMethods(cls) -> i32        (numero de metodos)
        //   getMethodAt(cls, i) -> i64    (MethodInfo* del i-esimo)
        //   getFields(cls) -> i32         (numero de fields de instancia)
        //   getFieldAt(cls, i) -> i64     (FieldInfo* del i-esimo)
        // Permiten descubrimiento dinamico sin conocer los nombres.
        if (id->name == "getMethods" || id->name == "getFields") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, id->name +
                    ": se espera 1 argumento (cls), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "getMethodAt" || id->name == "getFieldAt") {
            if (e->args.size() != 2) {
                diags_.error(e->loc, id->name +
                    ": se esperan 2 argumentos (cls, i), recibidos "
                    + std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "unloadmodule") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "unloadmodule: se espera 1 argumento (string literal con path), recibidos "
                    + std::to_string(e->args.size()));
            } else if (e->args[0] && e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                diags_.error(e->loc,
                    "unloadmodule: el path debe ser un string literal");
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "msgsend") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "msgsend: se esperan 2 argumentos (pid, valor i64), recibidos " +
                    std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "msgrecv") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "msgrecv: no acepta argumentos");
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // builtins de futures.
        //   future_alloc()         -> i64   (GcHandle del nuevo FutureObject PENDING)
        //   fulfill(fut, value)    -> void  (resuelve y despierta al waiter)
        // El await NO es un builtin sino una expresion (KW_AWAIT) procesada
        // en lower_expr.  Vease check de UnaryExpr para AwaitExpr o el caso
        // dedicado mas abajo si se anyade un AST node.
        if (id->name == "future_alloc") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "future_alloc: no acepta argumentos");
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        // loadmodule(string_lit) -> i64 (init_pc del modulo cargado, 0 = error).
        // Auto-invoca el main del modulo cargado tras la carga (callvm-equivalente
        // en el opcode loadmod), por lo que el __module_init del nuevo modulo se
        // ejecuta y sus clases quedan disponibles via findclass / forName tras
        // que loadmodule retorne.
        if (id->name == "loadmodule") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "loadmodule: se esperaba 1 argumento (string literal con la ruta), recibidos " +
                    std::to_string(e->args.size()));
            } else if (e->args[0]
                    && e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                diags_.error(e->loc,
                    "loadmodule: el argumento debe ser un string literal con la ruta al .velb");
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "fulfill") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    "fulfill: se esperan 2 argumentos (fut, valor i64), recibidos " +
                    std::to_string(e->args.size()));
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "wait" || id->name == "notify" || id->name == "notifyAll") {
            /* validacion estatica.  wait/notify/notifyAll requieren
             * mantener el monitor del target -- semanticamente solo tienen
             * sentido dentro de un bloque `synchronized (obj) { ... }`.
             * Llamarlas fuera produce IllegalMonitorState en runtime; el
             * check estatico evita el bug antes del primer arranque. */
            if (synchronized_depth_ == 0) {
                diags_.error(e->loc,
                    id->name + ": solo puede invocarse dentro de un bloque "
                    "'synchronized (obj) { ... }' (de lo contrario el proceso "
                    "no posee el monitor y se produce IllegalMonitorState en runtime)");
            }
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 argumento (target del monitor), recibidos " +
                    std::to_string(e->args.size()));
            }
            for (auto &a : e->args) {
                Type at = check_expr(a.get());
                if (at.kind != PrimitiveKind::CLASS
                 && at.kind != PrimitiveKind::COUNT) {
                    diags_.error(a->loc,
                        id->name + ": el argumento debe ser una referencia a clase, no '" +
                        type_to_string(at) + "'");
                }
            }
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "isPresent") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "isPresent: se esperaba 1 argumento, recibidos " +
                    std::to_string(e->args.size()));
            }
            for (auto &a : e->args) {
                Type at = check_expr(a.get());
                if (at.kind != PrimitiveKind::CLASS
                 && at.kind != PrimitiveKind::PTR
                 && at.kind != PrimitiveKind::I64
                 && at.kind != PrimitiveKind::OPTIONAL
                 && at.kind != PrimitiveKind::COUNT) {
                    diags_.error(a->loc,
                        "isPresent: el argumento debe ser Optional<T> o referencia, no '" +
                        type_to_string(at) + "'");
                }
            }
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "unwrap") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "unwrap: se esperaba 1 argumento, recibidos " +
                    std::to_string(e->args.size()));
            }
            Type at = e->args.empty() ? Type{}
                                       : check_expr(e->args[0].get());
            // Si el argumento es Optional<T>, devolvemos T (extrae el
            // payload).  Para CLASS/PTR seguimos el modelo nullable
            // legacy y devolvemos el mismo tipo.
            if (at.kind == PrimitiveKind::OPTIONAL && at.pointee) {
                Type rt = *at.pointee;
                e->result_type = rt;
                return rt;
            }
            if (at.kind != PrimitiveKind::CLASS
             && at.kind != PrimitiveKind::PTR
             && at.kind != PrimitiveKind::I64
             && at.kind != PrimitiveKind::COUNT) {
                diags_.error(e->loc,
                    "unwrap: el argumento debe ser una referencia o Optional<T>, no '" +
                    type_to_string(at) + "'");
            }
            e->result_type = at;
            return at;
        }
        // Builtins de Optional<T> (builtin del compilador).
        // Some(x) -> Optional<typeof(x)>: construye un Optional presente
        //           con el payload x.  El lowering emite un ALLOCA de 16
        //           bytes en stack + STORE 1 en +0 + STORE x en +8.
        if (id->name == "Some") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    "Some: se esperaba 1 argumento, recibidos " +
                    std::to_string(e->args.size()));
                e->result_type = Type{};
                return Type{};
            }
            // Sprint edge-bugs (2026-06-02): propagar expected_optional_type_
            // al inner cuando esperamos Optional<Optional<T>>.  Sin esto el
            // inner Some infiere T por defecto del literal (i64) en vez de
            // del contexto outer (Optional<i32>).
            const Type saved_outer_opt = expected_optional_type_;
            if (expected_optional_type_.kind == PrimitiveKind::OPTIONAL
             && expected_optional_type_.pointee
             && expected_optional_type_.pointee->kind == PrimitiveKind::OPTIONAL) {
                expected_optional_type_ = *expected_optional_type_.pointee;
            } else {
                /* Si el inner no es Optional, deshabilitar la propagacion
                 * para que el inner check_expr no la malinterprete. */
                expected_optional_type_ = Type{};
            }
            Type at = check_expr(e->args[0].get());
            expected_optional_type_ = saved_outer_opt;
            // Bug fix 2026-05-23: propagar el T esperado del Optional cuando
            // hay contexto.  Acepta el arg si es asignable al T esperado.
            Type final_t = at;
            if (expected_optional_type_.kind == PrimitiveKind::OPTIONAL
             && expected_optional_type_.pointee) {
                const Type &want = *expected_optional_type_.pointee;
                if (at.kind == PrimitiveKind::COUNT
                 || types_assignable(want, at)) {
                    final_t = want;
                }
            }
            Type rt = Type::make_optional(final_t);
            e->result_type = rt;
            return rt;
        }
        // None() -> Optional<T> donde T es del contexto si existe; sino i64.
        if (id->name == "None") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "None: no acepta argumentos");
            }
            Type t_inner{PrimitiveKind::I64};
            if (expected_optional_type_.kind == PrimitiveKind::OPTIONAL
             && expected_optional_type_.pointee) {
                t_inner = *expected_optional_type_.pointee;
            }
            Type rt = Type::make_optional(t_inner);
            e->result_type = rt;
            return rt;
        }
        // Builtins de Result<V, E>.
        // Ok(v) y Err(e) construyen Result<V, ?>; el tipo de error o
        // valor faltante debe inferirse del contexto (asignacion,
        // return).  Para MVP devolvemos Result con un placeholder; la
        // unificacion con el tipo declarado se hace en check_var_decl.
        if (id->name == "Ok") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "Ok: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type vt = check_expr(e->args[0].get());
            // Bug fix 2026-05-23: si hay contexto Result<V,E> esperado,
            // usar V y E del contexto en vez de placeholders.  V: si el arg
            // es asignable al V esperado (numerico permite coercion),
            // sobreescribir vt.  E: siempre tomar el E del contexto.
            Type final_v = vt;
            Type final_e{PrimitiveKind::I64};
            if (expected_result_type_.kind == PrimitiveKind::RESULT
             && expected_result_type_.pointee
             && expected_result_type_.pointee2) {
                const Type &want_v = *expected_result_type_.pointee;
                const Type &want_e = *expected_result_type_.pointee2;
                if (vt.kind == PrimitiveKind::COUNT
                 || types_assignable(want_v, vt)) {
                    final_v = want_v;
                }
                final_e = want_e;
            }
            Type rt = Type::make_result(final_v, final_e);
            e->result_type = rt;
            return rt;
        }
        if (id->name == "Err") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "Err: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            // Bug fix 2026-05-23: propagar contexto Result<V,E> al Err.
            // Antes de check_expr del arg, si el contexto E es STRING,
            // permitimos que el arg sea StringLitExpr (que normalmente seria
            // PTR).  El lowering hace la promotion via STRMAKE.
            Type et = check_expr(e->args[0].get());
            Type final_e = et;
            Type final_v{PrimitiveKind::I64};
            if (expected_result_type_.kind == PrimitiveKind::RESULT
             && expected_result_type_.pointee
             && expected_result_type_.pointee2) {
                const Type &want_v = *expected_result_type_.pointee;
                const Type &want_e = *expected_result_type_.pointee2;
                // Permitir coercion del arg al E esperado.
                if (et.kind == PrimitiveKind::COUNT
                 || types_assignable(want_e, et)
                 // Caso especial: literal string (PTR) -> STRING.
                 || (want_e.kind == PrimitiveKind::STRING
                     && et.kind == PrimitiveKind::PTR
                     && e->args[0]
                     && e->args[0]->kind == ast::NodeKind::StringLitExpr)) {
                    final_e = want_e;
                }
                final_v = want_v;
            }
            Type rt = Type::make_result(final_v, final_e);
            e->result_type = rt;
            return rt;
        }
        if (id->name == "isOk") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "isOk: se esperaba 1 argumento");
            } else {
                Type at = check_expr(e->args[0].get());
                if (at.kind != PrimitiveKind::RESULT
                 && at.kind != PrimitiveKind::COUNT) {
                    diags_.error(e->loc,
                        "isOk: el argumento debe ser Result<V,E>, no '" +
                        type_to_string(at) + "'");
                }
            }
            const Type rt{PrimitiveKind::I32};
            e->result_type = rt;
            return rt;
        }
        if (id->name == "value" || id->name == "error") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, id->name + ": se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            if (at.kind != PrimitiveKind::RESULT) {
                diags_.error(e->loc,
                    id->name + ": el argumento debe ser Result<V,E>, no '" +
                    type_to_string(at) + "'");
                e->result_type = Type{};
                return Type{};
            }
            Type rt = (id->name == "value")
                        ? (at.pointee  ? *at.pointee  : Type{})
                        : (at.pointee2 ? *at.pointee2 : Type{});
            e->result_type = rt;
            return rt;
        }

        // ===================================================================
        // Builtins de smart pointers: unique<T> y shared<T>.
        // ===================================================================
        //
        // Modelo de inferencia: estos builtins devuelven un tipo "generico"
        // con T = typeof(arg) (o U8 placeholder si el arg es count).  La
        // unificacion con el tipo declarado del LHS la hace check_var_decl
        // mediante types_assignable (que admite cualquier T compatible).
        //
        // `unique_box(value)` -> unique<typeof(value)>
        //   Aloca un slot host_ptr para `value` y lo guarda.  Deleter por
        //   defecto: `free` (Tier 0).  El lowering emite malloc(sizeof(T)) +
        //   STORE value + cleanup CALL free al exit del scope.
        if (id->name == "unique_box" || id->name == "shared_box") {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 argumento (valor a envolver)");
                e->result_type = Type{};
                return Type{};
            }
            // M7 / Opcion B: si el arg es un InitListExpr anonimo y el
            // contexto (return type de la funcion actual) es unique<T>/
            // shared<T> con T struct, anotar target_type_name antes del
            // check_expr para que el init list se resuelva contra T.
            // Cubre `return unique_box({.x=10, .y=20})` y similares fuera
            // de var-decl (donde check_var_decl ya anota).
            if (e->args[0]->kind == ast::NodeKind::InitListExpr) {
                auto *il = static_cast<ast::InitListExpr *>(e->args[0].get());
                if (il->target_type_name.empty()
                 && current_fn_return_type_.kind == (id->name == "unique_box"
                        ? PrimitiveKind::UNIQUE_PTR
                        : PrimitiveKind::SHARED_PTR)
                 && current_fn_return_type_.pointee
                 && current_fn_return_type_.pointee->kind == PrimitiveKind::STRUCT
                 && struct_layouts_.find(current_fn_return_type_.pointee->struct_name)
                    != struct_layouts_.end()) {
                    il->target_type_name = current_fn_return_type_.pointee->struct_name;
                }
            }
            Type vt = check_expr(e->args[0].get());
            if (vt.kind == PrimitiveKind::VOID
             || vt.kind == PrimitiveKind::COUNT) {
                diags_.error(e->loc,
                    id->name + ": tipo del valor invalido ('" +
                    type_to_string(vt) + "')");
            }
            Type rt = (id->name == "unique_box")
                ? Type::make_unique(vt)
                : Type::make_shared(vt);
            e->result_type = rt;
            return rt;
        }

        // ===================================================================
        // unique_with(value, deleter_fn) -> unique<typeof(value)>
        // shared_with(value, deleter_fn) -> shared<typeof(value)>
        //
        // Permite al programador especificar el alloc + dealloc para
        // cualquier recurso (memoria, archivos, sockets, handles OS, etc).
        // El primer argumento es el RESULTADO de la alocacion (ya hecho
        // por el usuario), y el segundo es el nombre de una funcion
        // (Vesta o extern) de aridad 1 que se invocara con el value al
        // exit del scope.
        //
        // Ejemplos:
        //   extern "kernel32" {
        //       fn VirtualAlloc(addr: u64, size: u64, t: u32, prot: u32) -> u64;
        //       fn VirtualFree(addr: u64, size: u64, type: u32) -> u32;
        //   }
        //   fn release_vmem(p: u64) { VirtualFree(p, 0, 0x8000); }
        //
        //   u64 mem = VirtualAlloc(0, 4096, 0x3000, 0x04);
        //   unique<i64> auto_mem = unique_with(mem, release_vmem);
        //   // ... usar auto_mem ... cleanup: release_vmem(mem) automatico
        // ===================================================================
        if (id->name == "unique_with" || id->name == "shared_with") {
            if (e->args.size() != 2) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 2 argumentos (value, deleter_fn)");
                e->result_type = Type{};
                return Type{};
            }
            Type vt = check_expr(e->args[0].get());
            if (vt.kind == PrimitiveKind::VOID
             || vt.kind == PrimitiveKind::COUNT) {
                diags_.error(e->loc,
                    id->name + ": tipo del valor invalido ('" +
                    type_to_string(vt) + "')");
            }
            // El segundo argumento debe ser IdentExpr de una funcion.
            if (e->args[1]->kind != ast::NodeKind::IdentExpr) {
                diags_.error(e->args[1]->loc,
                    id->name + ": el deleter debe ser un identificador de funcion (no una expresion)");
                e->result_type = Type{};
                return Type{};
            }
            auto *deleter_id = static_cast<ast::IdentExpr *>(e->args[1].get());
            const Symbol *del_sym = lookup(deleter_id->name);
            if (!del_sym) {
                diags_.error(e->args[1]->loc,
                    id->name + ": funcion deleter no declarada: '" +
                    deleter_id->name + "'");
                e->result_type = Type{};
                return Type{};
            }
            if (del_sym->kind != SymbolKind::Function) {
                diags_.error(e->args[1]->loc,
                    id->name + ": '" + deleter_id->name +
                    "' no es una funcion (es " +
                    (del_sym->kind == SymbolKind::Variable ? "variable" : "constante") + ")");
                e->result_type = Type{};
                return Type{};
            }
            const FunctionSig &sig = function_sigs_[del_sym->sig_index];
            if (sig.param_types.size() != 1) {
                diags_.error(e->args[1]->loc,
                    id->name + ": el deleter '" + deleter_id->name +
                    "' debe tener aridad 1, tiene " +
                    std::to_string(sig.param_types.size()));
                e->result_type = Type{};
                return Type{};
            }
            // Validar que el tipo del parametro del deleter sea compatible
            // con el value (laxa: aceptamos tipos numericos o ptr equivalentes).
            const Type &pt = sig.param_types[0];
            if (!types_assignable(pt, vt)
             && !(is_numeric(pt.kind) && is_numeric(vt.kind))) {
                diags_.error(e->args[1]->loc,
                    id->name + ": parametro del deleter '" +
                    type_to_string(pt) + "' incompatible con tipo del value '" +
                    type_to_string(vt) + "'");
            }
            // Marcamos el deleter_id para que el lowering sepa que es
            // referencia a funcion (no llamada).  Usamos result_type
            // FUNCTION para distinguir.  El lowering NO debe bajar este
            // IdentExpr a un valor; en su lugar lee el nombre y emite
            // el cleanup apropiado.
            deleter_id->result_type = Type::make_function(sig.param_types, sig.return_type);
            Type rt = (id->name == "unique_with")
                ? Type::make_unique(vt)
                : Type::make_shared(vt);
            e->result_type = rt;
            return rt;
        }

        // `move(p)` -> typeof(p): transfiere ownership.  El compilador
        // marca p como "consumed" via flag en lowering; un uso posterior
        // de p sera comprobado por el cleanup (binding = 0, skip).
        // Borrow checker R3: prohibe mover si p tiene borrows activos.
        // Z.8 builtins: atomic primitives + raw shared malloc/free.
        // atomic_load_i64(host_ptr) -> i64                    (acquire)
        // atomic_store_i64(host_ptr, val: i64) -> void        (release)
        // atomic_cas_i64(host_ptr, exp: i64, des: i64) -> i64 (acq_rel, retorna OLD)
        // atomic_add_i64(host_ptr, delta: i64) -> i64         (acq_rel, retorna OLD)
        // shared_malloc(size: u64) -> i64* (host_ptr) -- aloca en SharedHeap
        // shared_free(ptr: i64*) -> void
        if (id->name == "atomic_load_i64") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "atomic_load_i64: se esperaba 1 argumento (host_ptr)");
                e->result_type = Type{};
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            e->result_type = Type{PrimitiveKind::I64};
            return e->result_type;
        }
        if (id->name == "atomic_store_i64") {
            if (e->args.size() != 2) {
                diags_.error(e->loc, "atomic_store_i64: se esperaba 2 argumentos (host_ptr, val)");
                e->result_type = Type{};
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            (void)check_expr(e->args[1].get());
            e->result_type = Type{PrimitiveKind::VOID};
            return e->result_type;
        }
        if (id->name == "atomic_cas_i64") {
            if (e->args.size() != 3) {
                diags_.error(e->loc, "atomic_cas_i64: se esperaba 3 argumentos (host_ptr, exp, des)");
                e->result_type = Type{};
                return Type{};
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            e->result_type = Type{PrimitiveKind::I64};
            return e->result_type;
        }
        if (id->name == "atomic_add_i64") {
            if (e->args.size() != 2) {
                diags_.error(e->loc, "atomic_add_i64: se esperaba 2 argumentos (host_ptr, delta)");
                e->result_type = Type{};
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            (void)check_expr(e->args[1].get());
            e->result_type = Type{PrimitiveKind::I64};
            return e->result_type;
        }
        if (id->name == "shared_malloc") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "shared_malloc: se esperaba 1 argumento (size)");
                e->result_type = Type{};
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            // Tipo de retorno: i64* host (puntero raw a memoria shared).
            Type ret;
            ret.kind   = PrimitiveKind::PTR;
            ret.pointee = std::make_shared<Type>(Type{PrimitiveKind::I64});
            e->result_type = ret;
            return e->result_type;
        }
        if (id->name == "shared_free") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "shared_free: se esperaba 1 argumento (host_ptr)");
                e->result_type = Type{};
                return Type{};
            }
            (void)check_expr(e->args[0].get());
            e->result_type = Type{PrimitiveKind::VOID};
            return e->result_type;
        }

        // Z.10: introspeccion del SharedHeap.
        //   shared_heap_live_count() -> u32  (handles vivos en SharedHandleTable)
        //   shared_heap_bytes() -> u64       (total bytes alocados live)
        //   shared_gc_collect() -> void      (placeholder hasta Z.10-ext)
        if (id->name == "shared_heap_live_count") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "shared_heap_live_count: no acepta argumentos");
                e->result_type = Type{};
                return Type{};
            }
            e->result_type = Type{PrimitiveKind::U32};
            return e->result_type;
        }
        if (id->name == "shared_heap_bytes") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "shared_heap_bytes: no acepta argumentos");
                e->result_type = Type{};
                return Type{};
            }
            e->result_type = Type{PrimitiveKind::U64};
            return e->result_type;
        }
        if (id->name == "shared_gc_collect") {
            if (!e->args.empty()) {
                diags_.error(e->loc, "shared_gc_collect: no acepta argumentos");
                e->result_type = Type{};
                return Type{};
            }
            e->result_type = Type{PrimitiveKind::VOID};
            return e->result_type;
        }

        // Z.6 builtins: is_shared(obj) / share(obj) / unshare(obj).
        // - is_shared(obj) -> bool: chequea bit 31 del handle subyacente.
        //   Acepta cualquier CLASS / STRING / ARRAY (todo objeto GC).
        // - share(obj): promueve in-place al SharedHeap.  No-op si ya shared.
        //   Devuelve la misma referencia (mismo tipo).
        // - unshare(obj): deep-copy al gc_heap local.  Devuelve nueva ref local.
        //   Hoy v1 NO copia campos profundos (TODO Z.7 ext).
        if (id->name == "is_shared") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "is_shared: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            (void)at; // acepta cualquier tipo referencia
            e->result_type = Type{PrimitiveKind::BOOL};
            return e->result_type;
        }
        if (id->name == "share") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "share: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            // share devuelve el mismo tipo (la referencia ahora apunta al
            // SharedHeap pero el tipo logico no cambia).
            e->result_type = at;
            return at;
        }
        if (id->name == "unshare") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "unshare: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            e->result_type = at;
            return at;
        }

        if (id->name == "move") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "move: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            if (at.kind != PrimitiveKind::UNIQUE_PTR
             && at.kind != PrimitiveKind::SHARED_PTR
             && at.kind != PrimitiveKind::COUNT) {
                diags_.error(e->loc,
                    "move: el argumento debe ser unique<T> o shared<T>, no '" +
                    type_to_string(at) + "'");
                e->result_type = Type{};
                return Type{};
            }
            // Borrow checker R3: si el argumento es IdentExpr, validar
            // que no tenga borrows activos.
            if (e->args[0]->kind == ast::NodeKind::IdentExpr) {
                auto *idarg = static_cast<ast::IdentExpr *>(e->args[0].get());
                (void)borrow_checker_.on_owner_move(idarg->name, e->loc);
            }
            e->result_type = at;
            return at;
        }

        // `ptr_of(p)` -> T* host: extrae el puntero raw de un unique<T>
        // o shared<T> SIN consumir el smart pointer.  Para unique<T> es
        // p.ptr; para shared<T> es ctrl_block + 16 (offset del payload
        // inline).  El resultado es un T* host (movh).  Util para
        // operaciones que no deben extender la vida (e.g., pasar a una
        // funcion que no retiene el ptr).  Se llama `ptr_of` y no `get`
        // porque `get` ya es keyword reservada para properties (`get
        // name => expr`).
        if (id->name == "ptr_of") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "ptr_of: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            if (at.kind != PrimitiveKind::UNIQUE_PTR
             && at.kind != PrimitiveKind::SHARED_PTR) {
                diags_.error(e->loc,
                    "ptr_of: el argumento debe ser unique<T> o shared<T>, no '" +
                    type_to_string(at) + "'");
                e->result_type = Type{};
                return Type{};
            }
            // BugFix R2: para inner CLASS, devolver el tipo CLASS
            // directamente (no T*).  En Vex una instancia CLASS ya es un
            // host_ptr al ObjectHeader; unique<Class> guarda el host_ptr
            // directo sin doble indireccion (M7 in-place).  Asi
            // `ptr_of(unique<Resource>).method()` funciona naturalmente.
            // Para primitivos (i32, f64, etc.) seguimos retornando T* porque
            // unique<i32> aloca un buffer host de 4 bytes.
            if (at.pointee && at.pointee->kind == PrimitiveKind::CLASS) {
                Type rt = *at.pointee;
                e->result_type = rt;
                return rt;
            }
            // T* host (is_virtual=false por defecto).
            Type rt = Type::make_ptr(at.pointee ? *at.pointee : Type{}, false);
            e->result_type = rt;
            return rt;
        }

        // ===================================================================
        // Builtins de borrow checker: lend / lend_mut / read_borrow / write_borrow
        // ===================================================================
        //
        //   lend(owner)        -> borrow<T> (shared)
        //   lend_mut(owner)    -> borrow_mut<T> (exclusive)
        //   read_borrow(b)     -> T (lee el contenido apuntado)
        //   write_borrow(m, v) -> void (escribe a traves del mut borrow)
        //
        // lend/lend_mut: valida R1/R2 via BorrowChecker.  El argumento
        // debe ser un IdentExpr (no se permite tomar borrow de una
        // expresion compleja porque no hay un "owner" estable).
        if (id->name == "lend" || id->name == "lend_mut") {
            const bool is_mut = (id->name == "lend_mut");
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    id->name + ": se esperaba 1 argumento (el owner a prestar)");
                e->result_type = Type{};
                return Type{};
            }
            if (e->args[0]->kind != ast::NodeKind::IdentExpr) {
                diags_.error(e->args[0]->loc,
                    id->name + ": el argumento debe ser un identificador de variable (no una expresion)");
                e->result_type = Type{};
                return Type{};
            }
            auto *owner_id = static_cast<ast::IdentExpr *>(e->args[0].get());
            Type vt = check_expr(e->args[0].get());
            // bug3: rechazar lend(local_plain) en compile-time.
            // El modelo zero-cost del borrow checker requiere que el slot
            // del owner viva en HOST heap (host_ptr).  Para un local
            // primitivo declarado `i32 x = ...`, el slot vive en VM stack
            // (ALLOCA), no en host.  Si permitimos lend(local_plain), o
            // bien introducimos overhead runtime (RAW_ALLOC/RAW_FREE), o
            // bien rompemos la convencion uniforme borrow=host_ptr cuando
            // el borrow se pasa cross-funcion (la callee asumiria host
            // pero recibe VM addr -> corrupcion).
            //
            // Para mantener la promesa "borrow checker zero-cost", exigimos
            // que el owner sea unique<T>/shared<T> (host heap) o un borrow
            // anidado (reborrow).  Los locales primitivos plain deben
            // promocionarse explicitamente con `unique<T> x = unique_box(v)`.
            // Tipos cuyo "valor" YA es un host_ptr (lend zero-cost):
            //   UNIQUE_PTR     : slot 8B con host_ptr al payload.
            //   SHARED_PTR     : ctrl_block en GcHeap; lend devuelve payload@16.
            //   BORROW/_MUT    : reborrow; el inner ya es host_ptr.
            //   CLASS          : variable contiene host_ptr al objeto GC.
            //   PTR is_virtual=false: raw host pointer (T* via malloc o &heap).
            // Resto (i32, struct VM-stack, VirtualPtr<T>, etc.): error.
            const bool owner_is_host =
                vt.kind == PrimitiveKind::UNIQUE_PTR
             || vt.kind == PrimitiveKind::SHARED_PTR
             || vt.kind == PrimitiveKind::BORROW
             || vt.kind == PrimitiveKind::BORROW_MUT
             || vt.kind == PrimitiveKind::CLASS
             || ((vt.kind == PrimitiveKind::PTR
               || vt.kind == PrimitiveKind::ARRAY) && !vt.is_virtual);
            if (!owner_is_host
             && vt.kind != PrimitiveKind::COUNT
             && vt.kind != PrimitiveKind::VOID) {
                diags_.error(e->loc,
                    id->name + ": el owner '" + owner_id->name
                    + "' es un local plain.  El borrow checker zero-cost"
                    + " requiere que el owner viva en host heap: declara"
                    + " '" + owner_id->name + "' como `unique<"
                    + type_to_string(vt) + "> "
                    + owner_id->name + " = unique_box(...)`"
                    + " en lugar de un local primitivo.");
                e->result_type = Type{};
                return Type{};
            }
            // El tipo del borrow es borrow<T> donde T es el tipo
            // logico del owner.
            Type inner;
            if ((vt.kind == PrimitiveKind::UNIQUE_PTR
              || vt.kind == PrimitiveKind::SHARED_PTR)
              && vt.pointee) {
                inner = *vt.pointee;
            } else if ((vt.kind == PrimitiveKind::BORROW
                     || vt.kind == PrimitiveKind::BORROW_MUT)
                     && vt.pointee) {
                // F3 - reborrow: lend(borrow_var) -> shared borrow.
                // lend_mut(borrow_mut_var) -> mut reborrow.  Validamos
                // que el reborrow_mut solo se aplique a borrow_mut, no
                // a borrow (upgrade shared->mut prohibido).
                if (is_mut && vt.kind == PrimitiveKind::BORROW) {
                    diags_.error(e->loc,
                        "lend_mut: no se puede crear borrow_mut a partir de borrow shared (no se puede 'subir' la mutabilidad)");
                }
                inner = *vt.pointee;
            } else if ((vt.kind == PrimitiveKind::PTR
                     || vt.kind == PrimitiveKind::ARRAY)
                     && !vt.is_virtual && vt.pointee) {
                // Raw host pointer (T* o T[N] host): el inner es el pointee
                // (T), no el ptr mismo.  Semantica: lend(host_ptr_T) crea
                // borrow<T> que apunta al objeto pointed-to.
                inner = *vt.pointee;
            } else {
                inner = vt;
            }
            Type rt = Type::make_borrow(inner, is_mut);
            // F4 - propagar borrow_owner_source para lifetime tracking.
            // Si lend de un IdentExpr que es borrow: heredamos el source
            // (transitivo via reborrow).  Sino: el id es el owner directo.
            // IMPORTANTE: usamos @c borrow_owner_source de la expresion
            // (campo dedicado para tracking), NO @c Type::struct_name
            // que es parte de la identidad del tipo y romperia equality.
            if (vt.kind == PrimitiveKind::BORROW
             || vt.kind == PrimitiveKind::BORROW_MUT) {
                e->borrow_owner_source = borrow_checker_.root_owner_of(owner_id->name);
                if (e->borrow_owner_source.empty()) {
                    e->borrow_owner_source = owner_id->name;
                }
            } else {
                e->borrow_owner_source = owner_id->name;
            }
            // Registrar borrow en el borrow checker.  Para reborrow
            // (lend de un borrow_var), trazamos al owner root.
            std::string root_owner = owner_id->name;
            if (vt.kind == PrimitiveKind::BORROW
             || vt.kind == PrimitiveKind::BORROW_MUT) {
                // root_owner = lookup_root_owner(owner_id->name)
                // El borrow checker mantiene borrows_ con owner real.
                // Necesitamos exponer ese lookup.
                root_owner = borrow_checker_.root_owner_of(owner_id->name);
                if (root_owner.empty()) root_owner = owner_id->name;
            }
            // F3 ext - suspend semantics: si la fuente es un borrow_mut
            // activo, suspendemos su estado antes de @c on_lend para que
            // R1 (exclusividad mutable) no falle.  El estado se restaura
            // cuando el reborrow recien creado dropea (NLL o exit scope).
            //
            // Cubre dos casos:
            //   1) lend_mut(borrow_mut_var) = reborrow mut.
            //   2) lend(borrow_mut_var)     = shared reborrow (rebaja temporal).
            //
            // Si la fuente es un borrow shared (no mut) o el owner directo,
            // no necesitamos suspend: el reborrow shared simplemente
            // incrementa shared_count, y el lend de owner directo aplica
            // las reglas normales.
            const bool source_is_mut_borrow =
                (vt.kind == PrimitiveKind::BORROW_MUT);
            if (source_is_mut_borrow) {
                (void)borrow_checker_.suspend_for_reborrow(owner_id->name);
            }
            (void)borrow_checker_.on_lend(
                root_owner,
                /*borrower_name=*/"",   // VarDecl lo registra correctamente
                e->loc,
                is_mut);
            // F3 ext - marcar el binding pendiente como reborrow.  El
            // nombre real del reborrower se establece en check_var_decl;
            // alli leemos @c borrow_owner_source y comparamos con la
            // fuente.  Aqui guardamos la info en el AST node para que
            // check_var_decl pueda recuperarla sin re-analizar.
            if (source_is_mut_borrow) {
                e->borrow_reborrow_source_is_mut = true;
                e->borrow_reborrow_source_name   = owner_id->name;
            }
            e->result_type = rt;
            return rt;
        }

        // read_borrow(b) -> T: lee el valor a traves del borrow.
        // Equivalente conceptual a `*b` (deref).  No requiere que el
        // borrow sea mut.
        if (id->name == "read_borrow") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "read_borrow: se esperaba 1 argumento (un borrow)");
                e->result_type = Type{};
                return Type{};
            }
            Type bt = check_expr(e->args[0].get());
            if (bt.kind != PrimitiveKind::BORROW
             && bt.kind != PrimitiveKind::BORROW_MUT) {
                diags_.error(e->args[0]->loc,
                    "read_borrow: el argumento debe ser borrow<T> o borrow_mut<T>, no '" +
                    type_to_string(bt) + "'");
                e->result_type = Type{};
                return Type{};
            }
            Type rt = bt.pointee ? *bt.pointee : Type{};
            e->result_type = rt;
            return rt;
        }

        // write_borrow(m, v) -> void: escribe a traves del mut borrow.
        // Solo admite borrow_mut<T>.
        if (id->name == "write_borrow") {
            if (e->args.size() != 2) {
                diags_.error(e->loc, "write_borrow: se esperaba 2 argumentos (borrow_mut, value)");
                e->result_type = Type{PrimitiveKind::VOID};
                return Type{PrimitiveKind::VOID};
            }
            Type bt = check_expr(e->args[0].get());
            Type vt = check_expr(e->args[1].get());
            if (bt.kind != PrimitiveKind::BORROW_MUT) {
                diags_.error(e->args[0]->loc,
                    "write_borrow: el primer argumento debe ser borrow_mut<T>, no '" +
                    type_to_string(bt) + "'");
            }
            if (bt.pointee && !types_assignable(*bt.pointee, vt)) {
                diags_.error(e->args[1]->loc,
                    "write_borrow: tipo del valor (" + type_to_string(vt) +
                    ") incompatible con el tipo del borrow (" +
                    type_to_string(bt.pointee ? *bt.pointee : Type{}) + ")");
            }
            const Type rt{PrimitiveKind::VOID};
            e->result_type = rt;
            return rt;
        }

        // `use_count(s)` -> i64: refcount actual del shared<T>.  Util
        // para diagnostico; cero si el shared esta moved.  En MVP la
        // operacion lee directamente el campo refcount del control block.
        if (id->name == "use_count") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "use_count: se esperaba 1 argumento");
                e->result_type = Type{};
                return Type{};
            }
            Type at = check_expr(e->args[0].get());
            if (at.kind != PrimitiveKind::SHARED_PTR) {
                diags_.error(e->loc,
                    "use_count: el argumento debe ser shared<T>, no '" +
                    type_to_string(at) + "'");
            }
            const Type rt{PrimitiveKind::I64};
            e->result_type = rt;
            return rt;
        }

        /* A.43.16: @Macro -- comptime fn cuyo string de retorno se
         * INYECTA como codigo Vex en el call site (auto-emit).  El
         * call site evalua la fn al compile-time, parsea el resultado
         * como expresion y type-checa la expresion en el contexto
         * actual.  El tipo retornado por el call es el de la expresion
         * generada, NO `string` (el tipo declarado de la fn). */
        {
            auto fn_it = comptime_fns_.find(id->name);
            if (fn_it != comptime_fns_.end()
             && fn_it->second && fn_it->second->is_macro) {
                /* Phase MC.9/MC.10: VM eval es el camino DEFAULT cuando
                 * el bytecode esta disponible.  Sin flags ni opt-in: si
                 * @c comptime_runtime_ tiene el macro registrado y los
                 * args son codificables como uint64, invocamos via VM.
                 * Fallback transparente al AST evaluator si la VM falla
                 * o si los args no son encodables.  El bytecode se
                 * popula automaticamente via two-phase compile (main.cpp
                 * orquestador) sin intervencion del usuario. */
                ComptimeEvalResult r;
                bool used_vm = false;
                if ((comptime_runtime_.registered_macro_count() > 0)
                 && e->args.size() <= 12) {
                    std::vector<uint64_t> arg_words;
                    arg_words.reserve(e->args.size());
                    bool can_encode = true;
                    for (const auto &a : e->args) {
                        if (!a) { can_encode = false; break; }
                        switch (a->kind) {
                            case ast::NodeKind::IntLitExpr: {
                                const auto *lit = static_cast<const ast::IntLitExpr *>(a.get());
                                arg_words.push_back(lit->value);
                                break;
                            }
                            case ast::NodeKind::BoolLitExpr: {
                                const auto *lit = static_cast<const ast::BoolLitExpr *>(a.get());
                                arg_words.push_back(lit->value ? 1u : 0u);
                                break;
                            }
                            case ast::NodeKind::CharLitExpr: {
                                const auto *lit = static_cast<const ast::CharLitExpr *>(a.get());
                                arg_words.push_back(lit->codepoint);
                                break;
                            }
                            case ast::NodeKind::NullLitExpr:
                                arg_words.push_back(0u);
                                break;
                            case ast::NodeKind::FloatLitExpr: {
                                /* f64 literal -> bits IEEE 754 en u64
                                 * (bitcast).  El body del macro lee el reg
                                 * como bits y reconstruye el f64 via FCVT/
                                 * BITCAST IR si es necesario. */
                                const auto *lit = static_cast<const ast::FloatLitExpr *>(a.get());
                                uint64_t bits = 0;
                                std::memcpy(&bits, &lit->value, sizeof(bits));
                                arg_words.push_back(bits);
                                break;
                            }
                            case ast::NodeKind::StringLitExpr: {
                                /* string literal -> GcHandle a un
                                 * StringObject construido via
                                 * @c runtime::make_string_flat (misma maquinaria
                                 * que STRMAKE).  Solo soportamos literales
                                 * NO interpolados; los interpolados requieren
                                 * runtime evaluation que no podemos pre-computar. */
                                const auto *lit = static_cast<const ast::StringLitExpr *>(a.get());
                                if (lit->is_interpolated()) {
                                    can_encode = false;
                                    break;
                                }
                                uint64_t handle = 0;
                                if (!comptime_runtime_.marshal_string(lit->value, handle)) {
                                    can_encode = false;
                                    break;
                                }
                                arg_words.push_back(handle);
                                break;
                            }
                            case ast::NodeKind::UnaryExpr: {
                                /* -literal -> negacion compile-time.
                                 * Cubre patrones comunes como `M(-42)` que
                                 * el parser representa como UnaryExpr(Neg,
                                 * IntLit(42)). */
                                const auto *u = static_cast<const ast::UnaryExpr *>(a.get());
                                if (u->op == ast::UnOp::Neg && u->operand) {
                                    if (u->operand->kind == ast::NodeKind::IntLitExpr) {
                                        const auto *lit = static_cast<const ast::IntLitExpr *>(u->operand.get());
                                        const int64_t signed_val =
                                            -static_cast<int64_t>(lit->value);
                                        arg_words.push_back(static_cast<uint64_t>(signed_val));
                                    } else if (u->operand->kind == ast::NodeKind::FloatLitExpr) {
                                        const auto *lit = static_cast<const ast::FloatLitExpr *>(u->operand.get());
                                        const double neg = -lit->value;
                                        uint64_t bits = 0;
                                        std::memcpy(&bits, &neg, sizeof(bits));
                                        arg_words.push_back(bits);
                                    } else {
                                        can_encode = false;
                                    }
                                } else {
                                    can_encode = false;
                                }
                                break;
                            }
                            default:
                                can_encode = false;
                                break;
                        }
                        if (!can_encode) break;
                    }
                    if (can_encode) {
                        std::string vm_out;
                        /* si el macro es @Pure, usar la
                         * variante memoized (cache HOST-side @c
                         * (macro,args) -> result).  Hits del cache no
                         * tocan la VM.  Si el macro no es @Pure, no
                         * cache; cada call site invoca el VM fresco. */
                        const bool is_pure = fn_it->second->is_pure;
                        const bool vm_ok = comptime_runtime_
                            .invoke_string_macro_memoized(
                                "__macro_" + id->name,
                                arg_words, vm_out, is_pure);
                        if (vm_ok) {
                            r.ok      = true;
                            r.is_str  = true;
                            r.str     = std::move(vm_out);
                            used_vm   = true;
                            ++macro_vmonly_hits_;
                        } else {
                            ++macro_vmonly_misses_;
                        }
                    }
                }
                /* Fallback AST: corre solo si VM-only no aplico o fallo. */
                if (!used_vm) {
                    r = comptime_eval_expr(*this, e);
                }
                if (!r.ok || !r.is_str) {
                    diags_.error(e->loc,
                        "@Macro '" + id->name + "' debe ser comptime-evaluable a string");
                    return Type{};
                }
                /* Parsear el string como expresion Vex. */
                Lexer fragment_lex(r.str, "<macro:" + id->name + ">", diags_);
                Parser fragment_par(fragment_lex, diags_);
                std::unique_ptr<ast::Expr> parsed = fragment_par.parse_one_expr();
                if (!parsed) {
                    diags_.error(e->loc,
                        "@Macro '" + id->name + "' devolvio codigo no-parseable: " + r.str);
                    return Type{};
                }
                parsed->loc = e->loc;
                /* Type-checar y guardar el AST para que el lowering lo recoja. */
                const Type rt = check_expr(parsed.get());
                e->macro_expanded = std::move(parsed);
                e->result_type    = rt;

                /* record SHADOW EXPECTATION para validacion
                 * cruzada AST vs VM.  Solo registramos si:
                 *   (a) Todos los args son literales codificables como
                 *       uint64 (Int, Bool, Char, Null) -- el VM espera
                 *       valores raw en R1..R12.  Otros tipos requeririan
                 *       marshalling adicional que cae fuera del scope MC.8.
                 *   (b) Hay <= 12 args (CALLVM convention).
                 *
                 * Si no encaja, simplemente no registramos -- la expansion
                 * AST sigue siendo correcta y el shadow_validate solo
                 * cubrira un subset de call sites en MC.8.  MC.9 ampliara
                 * el marshalling para string args, structs, etc. */
                if (e->args.size() <= 12) {
                    std::vector<uint64_t> arg_words;
                    arg_words.reserve(e->args.size());
                    bool can_record = true;
                    for (const auto &a : e->args) {
                        if (!a) { can_record = false; break; }
                        switch (a->kind) {
                            case ast::NodeKind::IntLitExpr: {
                                const auto *lit = static_cast<const ast::IntLitExpr *>(a.get());
                                arg_words.push_back(lit->value);
                                break;
                            }
                            case ast::NodeKind::BoolLitExpr: {
                                const auto *lit = static_cast<const ast::BoolLitExpr *>(a.get());
                                arg_words.push_back(lit->value ? 1u : 0u);
                                break;
                            }
                            case ast::NodeKind::CharLitExpr: {
                                const auto *lit = static_cast<const ast::CharLitExpr *>(a.get());
                                arg_words.push_back(lit->codepoint);
                                break;
                            }
                            case ast::NodeKind::NullLitExpr:
                                arg_words.push_back(0u);
                                break;
                            case ast::NodeKind::FloatLitExpr: {
                                /* bitcast double -> u64. */
                                const auto *lit = static_cast<const ast::FloatLitExpr *>(a.get());
                                uint64_t bits = 0;
                                std::memcpy(&bits, &lit->value, sizeof(bits));
                                arg_words.push_back(bits);
                                break;
                            }
                            case ast::NodeKind::UnaryExpr: {
                                /* -literal -> negacion comptime. */
                                const auto *u = static_cast<const ast::UnaryExpr *>(a.get());
                                if (u->op == ast::UnOp::Neg && u->operand) {
                                    if (u->operand->kind == ast::NodeKind::IntLitExpr) {
                                        const auto *lit = static_cast<const ast::IntLitExpr *>(u->operand.get());
                                        const int64_t sv = -static_cast<int64_t>(lit->value);
                                        arg_words.push_back(static_cast<uint64_t>(sv));
                                    } else if (u->operand->kind == ast::NodeKind::FloatLitExpr) {
                                        const auto *lit = static_cast<const ast::FloatLitExpr *>(u->operand.get());
                                        const double neg = -lit->value;
                                        uint64_t bits = 0;
                                        std::memcpy(&bits, &neg, sizeof(bits));
                                        arg_words.push_back(bits);
                                    } else {
                                        can_record = false;
                                    }
                                } else {
                                    can_record = false;
                                }
                                break;
                            }
                            default:
                                can_record = false;
                                break;
                        }
                        if (!can_record) break;
                    }
                    if (can_record) {
                        const std::string src_loc =
                            e->loc.file + ":" +
                            std::to_string(e->loc.line) + ":" +
                            std::to_string(e->loc.column);
                        comptime_runtime_.record_expectation(
                            id->name, std::move(arg_words), r.str, src_loc);
                    }
                }
                return rt;
            }
        }

        size_t id_depth = 0;
        const Symbol *s = lookup_with_depth(id->name, &id_depth);
        if (!s) {
            diags_.error(e->loc, "funcion no declarada: '" + id->name + "'");
            for (auto &a : e->args) (void)check_expr(a.get());
            return Type{};
        }
        // closures: si el simbolo es una variable de tipo FUNCTION
        // (function pointer / closure), tratamos esto como llamada
        // indirecta y devolvemos el return type del tipo.  Si la lambda
        // esta en stack del scope actual, no necesitamos captura; si la
        // variable vive en un scope exterior y estamos dentro de otra
        // lambda, registramos la captura igual que para variables
        // ordinarias (la rama esta en check_ident, pero aqui no pasamos
        // por check_ident, asi que replicamos la logica).
        if (s->kind != SymbolKind::Function
         && s->type.kind == PrimitiveKind::FUNCTION) {
            // Captura como variable normal si aplica.
            if (!lambda_stack_.empty()) {
                for (auto &ctx : lambda_stack_) {
                    if (id_depth < ctx.outer_depth) {
                        bool already = false;
                        for (auto &nm : ctx.expr->captures) {
                            if (nm == id->name) { already = true; break; }
                        }
                        if (!already) {
                            ctx.expr->captures.push_back(id->name);
                            ctx.expr->capture_types.push_back(s->type);
                        }
                    }
                }
            }
            const Type fn_type = s->type;
            // Validar aridad y tipos de los argumentos contra fn_params.
            if (e->args.size() != fn_type.fn_params.size()) {
                diags_.error(e->loc,
                    std::string("numero de argumentos incorrecto en llamada al closure '") +
                    id->name + "': esperados " + std::to_string(fn_type.fn_params.size()) +
                    ", recibidos " + std::to_string(e->args.size()));
            }
            const size_t n = std::min(e->args.size(), fn_type.fn_params.size());
            for (size_t i = 0; i < n; ++i) {
                Type ta = check_expr(e->args[i].get());
                const Type &tp = fn_type.fn_params[i];
                // Promocion automatica de funcion top-level a function
                // value cuando se pasa por nombre como argumento:  si el
                // parametro espera FUNCTION y el argumento es un identifier
                // que resuelve a una funcion declarada en el modulo con
                // firma compatible, sintetizar un function value
                // (fn_addr, env_addr=0).  Patcheamos result_type del
                // IdentExpr para que el lowering lo
                // reconozca y emita el slot de 16 bytes con env=0.
                if (tp.kind == PrimitiveKind::FUNCTION
                 && ta.kind == PrimitiveKind::VOID
                 && e->args[i]->kind == ast::NodeKind::IdentExpr) {
                    auto *id_arg = static_cast<ast::IdentExpr *>(e->args[i].get());
                    const Symbol *s_arg = lookup(id_arg->name);
                    if (s_arg && s_arg->kind == SymbolKind::Function) {
                        const FunctionSig &arg_sig = function_sigs_[s_arg->sig_index];
                        Type fnv = Type::make_function(arg_sig.param_types,
                                                       arg_sig.return_type);
                        if (types_assignable(tp, fnv)) {
                            ta              = fnv;
                            id_arg->result_type = fnv;
                        }
                    }
                }
                if (ta.kind != PrimitiveKind::COUNT && !types_assignable(tp, ta)) {
                    diags_.error(e->args[i]->loc,
                        std::string("argumento ") + std::to_string(i + 1) +
                        ": tipo (" + type_to_string(ta) +
                        ") incompatible con parametro (" + type_to_string(tp) + ")");
                }
            }
            for (size_t i = n; i < e->args.size(); ++i) (void)check_expr(e->args[i].get());
            // Marcar el callee con el tipo function para que el lowering
            // sepa que es una llamada indirecta a closure.
            e->callee->result_type = fn_type;
            return fn_type.pointee ? *fn_type.pointee : Type{PrimitiveKind::VOID};
        }
        if (s->kind != SymbolKind::Function) {
            diags_.error(e->loc, "'" + id->name + "' no es una funcion");
            for (auto &a : e->args) (void)check_expr(a.get());
            return Type{};
        }
        const FunctionSig &sig = function_sigs_[s->sig_index];

        // dispose(xs) acepta cualquier tipo coleccion (no solo I64).
        // Validamos que el arg es un IdentExpr (necesario en el lowering
        // para reescribir el local) y que su tipo es uno de los tipos
        // coleccion primitivos.
        if (id->name == "dispose") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "'dispose' espera exactamente 1 argumento");
            } else {
                Type ta = check_expr(e->args[0].get());
                if (!is_col_kind(ta.kind)) {
                    diags_.error(e->args[0]->loc,
                        std::string("'dispose' requiere un tipo coleccion, recibido ") +
                        type_to_string(ta));
                }
                if (e->args[0]->kind != ast::NodeKind::IdentExpr) {
                    diags_.error(e->args[0]->loc,
                        "'dispose' requiere un identificador local (no expresion compuesta)");
                }
            }
            return Type{PrimitiveKind::VOID};
        }

        // ffi_call(fn, ...) es variadic 1+0..12.  Bypass del strict
        // type-check de aridad.  El primer arg debe ser i64 (puntero a
        // funcion); los siguientes (0..12) son argumentos opacos i64 que
        // el lowering empaqueta en R01..R12 antes del callni.  Cada uno
        // se chequea con check_expr para detectar errores semanticos
        // basicos pero no se valida tipo (todos se tratan como i64 en
        // la calling convention nativa).
        if (id->name == "ffi_call") {
            if (e->args.empty()) {
                diags_.error(e->loc,
                    "'ffi_call' requiere al menos 1 arg (puntero a funcion)");
            } else if (e->args.size() > 13) {
                diags_.error(e->loc,
                    "'ffi_call' acepta como maximo 12 args ademas del puntero");
            }
            for (auto &a : e->args) (void)check_expr(a.get());
            return sig.return_type;
        }

        // print/println/echo aceptan cualquier tipo escalar como
        // arg[0] (despacha en lowering).  Bypass del strict type-check.
        // print_ptr/print_gchandle tambien son polimorficos: aceptan
        // CLASS, PTR, ARRAY, o entero arbitrario sin que el bypass del
        // type checker se queje del tipo.  El lowering despacha al
        // CALLN apropiado convirtiendo a uint64.
        /* Sprint B.1: as_native_callback(fn_name) bypass.  Acepta IdentExpr
         * que resuelve a Function (no a Variable).  Captura el nombre +
         * argc para que el lowering emita la secuencia correcta. */
        if (id->name == "as_native_callback") {
            if (e->args.size() != 1) {
                diags_.error(e->loc, "'as_native_callback' espera 1 argumento (nombre de funcion Vex)");
                return Type{PrimitiveKind::I64};
            }
            auto *fn_id = dynamic_cast<ast::IdentExpr *>(e->args[0].get());
            if (fn_id == nullptr) {
                diags_.error(e->args[0]->loc,
                    "'as_native_callback' requiere un identificador de funcion (no expresion)");
                return Type{PrimitiveKind::I64};
            }
            /* Lookup del simbolo via lookup_with_depth. */
            size_t depth = 0;
            const Symbol *sym = lookup_with_depth(fn_id->name, &depth);
            if (sym == nullptr || sym->kind != SymbolKind::Function) {
                diags_.error(fn_id->loc,
                    "'as_native_callback': '" + fn_id->name + "' no es una funcion conocida");
                return Type{PrimitiveKind::I64};
            }
            /* Marcamos result_type del IdentExpr como I64 sentinela para
             * que el lowering reconozca el patron (sin pasar por lower_ident
             * normal que daria error de simbolo). */
            fn_id->result_type = Type{PrimitiveKind::I64};
            return Type{PrimitiveKind::I64};
        }

        const bool is_io_print_relaxed =
            (id->name == "print" || id->name == "println" || id->name == "echo"
          || id->name == "print_ptr" || id->name == "print_gchandle");
        if (is_io_print_relaxed) {
            if (e->args.size() != 1) {
                diags_.error(e->loc,
                    std::string("'") + id->name +
                    "' espera exactamente 1 argumento");
            } else {
                Type ta = check_expr(e->args[0].get());
                if (ta.kind == PrimitiveKind::VOID
                 || ta.kind == PrimitiveKind::COUNT) {
                    diags_.error(e->args[0]->loc,
                        std::string("'") + id->name +
                        "' no acepta argumentos de tipo void");
                }
            }
            return sig.return_type;
        }

        // Aridad.
        if (e->args.size() != sig.param_types.size()) {
            diags_.error(e->loc,
                std::string("numero de argumentos incorrecto en llamada a '") + id->name +
                "': esperados " + std::to_string(sig.param_types.size()) +
                ", recibidos " + std::to_string(e->args.size()));
        }
        // Tipos de cada arg.  Usamos types_assignable para admitir las
        // mismas conversiones implicitas que en var-decl/asignacion
        // (numericos entre si, void* <-> T*).  Para builtins generic-like
        // como free(PTR), el parametro declarado es PTR sin pointee
        // (equivalente a void*) y types_assignable acepta cualquier T*.
        const size_t n = std::min(e->args.size(), sig.param_types.size());
        for (size_t i = 0; i < n; ++i) {
            Type ta = check_expr(e->args[i].get());
            const Type tp = sig.param_types[i];
            // Coercion gap N : mismo patron que en el closure-call
            // de arriba.  Si el parametro espera FUNCTION y el argumento
            // es un identifier que resuelve a funcion top-level, lo
            // promovemos a function value (fn_addr, env_addr=0).
            if (tp.kind == PrimitiveKind::FUNCTION
             && ta.kind == PrimitiveKind::VOID
             && e->args[i]->kind == ast::NodeKind::IdentExpr) {
                auto *id_arg = static_cast<ast::IdentExpr *>(e->args[i].get());
                const Symbol *s_arg = lookup(id_arg->name);
                if (s_arg && s_arg->kind == SymbolKind::Function) {
                    const FunctionSig &arg_sig = function_sigs_[s_arg->sig_index];
                    Type fnv = Type::make_function(arg_sig.param_types,
                                                   arg_sig.return_type);
                    if (types_assignable(tp, fnv)) {
                        ta              = fnv;
                        id_arg->result_type = fnv;
                    }
                }
            }
            if (ta.kind != PrimitiveKind::COUNT && !types_assignable(tp, ta)) {
                diags_.error(e->args[i]->loc,
                    std::string("argumento ") + std::to_string(i + 1) +
                    ": tipo (" + type_to_string(ta) +
                    ") incompatible con parametro (" + type_to_string(tp) + ")");
            }
        }
        // Chequear los argumentos extra para sus efectos (si la aridad fallo).
        for (size_t i = n; i < e->args.size(); ++i) (void)check_expr(e->args[i].get());

        // F4 - lifetime elision rule 1: si la funcion devuelve borrow<T>
        // o borrow_mut<T> y tiene EXACTAMENTE un parametro borrow (o un
        // self CLASS implicito), el lifetime del retorno = lifetime de
        // ese argumento.  Propagamos el borrow_owner_source del arg a
        // la expresion CallExpr para que el caller pueda registrar el
        // nuevo borrow con el owner correcto.
        const Type &rty = sig.return_type;
        const bool ret_is_borrow = (rty.kind == PrimitiveKind::BORROW
                                 || rty.kind == PrimitiveKind::BORROW_MUT);
        if (ret_is_borrow) {
            size_t borrow_param_idx = SIZE_MAX;
            size_t borrow_param_count = 0;
            for (size_t i = 0; i < sig.param_types.size(); ++i) {
                if (sig.param_types[i].kind == PrimitiveKind::BORROW
                 || sig.param_types[i].kind == PrimitiveKind::BORROW_MUT) {
                    borrow_param_idx = i;
                    borrow_param_count++;
                }
            }
            if (borrow_param_count == 1 && borrow_param_idx < e->args.size()) {
                // Heredamos el source del arg correspondiente.
                const std::string &src = e->args[borrow_param_idx]->borrow_owner_source;
                if (!src.empty()) {
                    e->borrow_owner_source = src;
                }
            } else if (borrow_param_count > 1) {
                diags_.warning(e->loc,
                    "lifetime elision ambigua: la funcion '" + id->name +
                    "' tiene multiples parametros borrow; la elision rule 1 no aplica.\n"
                    "  El borrow retornado podria tener cualquiera de los lifetimes.\n"
                    "  (Anotaciones explicitas no soportadas; considera reescribir.)");
            }
        }

        return sig.return_type;
    }

} // namespace vex
