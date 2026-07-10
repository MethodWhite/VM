/*
 * type_checker.cpp - Type checking pass for Vex language
 *
 * SECTION MAP:
 *   Lines     1-  236: Static helpers, extern "C" functions, constructor
 *   Lines   237-  266: Destructor
 *   Lines   267-  972: Mangle functions, utility functions
 *   Lines   973- 1246: class_is_assignable
 *   Lines  1247- 1513: TypeChecker::run()
 *   Lines  1514- 1833: Scope management, type_from_node helper
 *   Lines  1834- 3101: collect_globals() - first pass
 *   Lines  3102- 3602: check_functions() - second pass
 *   Lines  3603- 4099: check_class_method, check_block, check_stmt
 *   Lines  4100- 4554: check_var_decl
 *   Lines  4555- 4734: if/while/for/return statement checking
 *   Lines  4735- 6815: Expression checking (check_expr and helpers)
 *   Lines  6816- 9729: check_call + generic monomorphization
 *   Lines  9730:      Namespace close
 *
 * To split: extract each range into a separate .cpp file under
 * namespace vex { ... } with #include "vex/type_checker.h"
 * and register in src/vex/CMakeLists.txt
 */
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
#include <functional>
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

    /* non-static (visible desde type_checker_expr.cpp) */
    std::string mangle_args(const std::vector<Type> &args) {
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

} // namespace vex
