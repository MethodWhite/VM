/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file vex/comptime_introspect.h
 * @brief Helpers compartidos por type checker y lowering para resolver
 *        los builtins comptime de introspection (sizeof<T>, alignof<T>,
 *        typename<T>, type_id<T>, kind<T>, ...).
 *
 * El type checker valida (aridad, tipo de retorno) y el lowering computa
 * el valor constante a emitir como CONST IR.  Ambos comparten estos
 * helpers para evitar drift entre validacion y emision.
 *
 * (compile-time) de la introspection: cero overhead.  Cada llamada
 * a un builtin aqui se baja a UN solo @c IrOp::CONST en el IR -- no genera
 * codigo runtime, no consulta tablas, no allocates.  Si @c T no es
 * resoluble estaticamente, el type checker emite error y nunca se llega al
 * lowering.
 */

#ifndef VEX_COMPTIME_INTROSPECT_H
#define VEX_COMPTIME_INTROSPECT_H

#include "vex/types.h"
#include "vex/type_checker.h"
#include "vex/ast.h"

#include <cstdint>
#include <string>

namespace vex {

    /**
     * @brief Codigos del builtin @c kind<T>().
     *
     * Devuelto como i32 al codigo Vex.  Los valores son ESTABLES entre
     * builds: si se anyade un kind nuevo, va al final de la lista para
     * no romper codigo existente que cheque kinds via comparacion entera.
     */
    enum class ComptimeKind : int32_t {
        Primitive   = 0,   /// i8..u64, f32/f64, bool, char, void
        Class       = 1,   /// reference type (host_ptr al ObjectHeader)
        Struct      = 2,   /// value type inline
        Enum        = 3,   /// ADT con variantes
        Optional    = 4,   /// Optional<T>
        Result      = 5,   /// Result<V,E>
        Array       = 6,   /// T[N] o T[]
        Ptr         = 7,   /// T*
        Function    = 8,   /// fn(...) -> R
        String      = 9,   /// string (GcHandle a StringObject)
        Borrow      = 10,  /// borrow<T> o borrow_mut<T>
        Future      = 11,  /// Future<T>
        Unique      = 12,  /// unique<T>
        Shared      = 13,  /// shared<T>
        Collection  = 14,  /// ArrayList, HashMap, etc.
        Unknown     = 99,  /// no resoluble (defensivo; el checker emite error antes)
    };

    /**
     * @brief Tamano en bytes de un tipo Vex resuelto.
     *
     * Para PRIMITIVE / OPTIONAL / RESULT / FUNCTION / STRING / PTR el valor
     * sale de @c primitive_size_bytes.  Para STRUCT / CLASS / ARRAY / ENUM
     * consulta el layout en el type checker (struct_layouts / class_layouts /
     * enum_layouts).
     *
     * @param tc Type checker con los layouts ya construidos (fase de check
     *           completa).
     * @param t  Tipo a medir.
     * @return Tamano en bytes; 0 si no resoluble (caller debe emitir error).
     *
     * Nota: para CLASS devuelve 8 (es un reference type -- la variable
     * guarda un puntero al ObjectHeader, ocupa 8 bytes; el tamano de la
     * INSTANCIA es otra metrica, accesible via @c kind<T>()==Class +
     * instance_size que se expondra en una API runtime).
     */
    uint64_t comptime_type_size(const TypeChecker &tc, const Type &t);

    /**
     * @brief Alineamiento en bytes de un tipo Vex.
     *
     * Para primitivos = sizeof (excepto BOOL/CHAR/I8 = 1).  Para STRUCT
     * usa @c StructLayout::align_bytes.  Para CLASS = 8 (puntero alineado).
     */
    uint64_t comptime_type_align(const TypeChecker &tc, const Type &t);

    /**
     * @brief Nombre canonico del tipo para @c typename<T>().
     *
     * Primitivos: "i32", "u64", "f64", "bool", "string", etc.  Compuestos:
     * "Point" (struct/class/enum), "Box<i32>" (generico monomorphizado),
     * "i32*" (ptr), "i32[8]" (array fija).  Sin espacios.
     */
    std::string comptime_type_name(const TypeChecker &tc, const Type &t);

    /**
     * @brief ID estable (FNV-1a 32-bit del nombre canonico) para
     *        @c type_id<T>().
     *
     * Igual ID = mismo tipo logico cross-compile mientras el nombre canonico
     * no cambie.  Util para fast-path "es el tipo X?" via comparacion entera
     * (1 cmp + 1 jcc) en vez de comparar nombres.
     */
    uint32_t comptime_type_id(const TypeChecker &tc, const Type &t);

    /**
     * @brief Categoria de tipo para @c kind<T>().
     */
    ComptimeKind comptime_type_kind(const Type &t);

    // -------------------------------------------------------------------
    // Sprint 2: queries de fields y methods (compile-time).
    // -------------------------------------------------------------------

    /**
     * @brief Offset en bytes del campo @c field_name dentro de @c t.
     *
     * @return Offset >= 0 si el campo existe; -1 si no existe o @c t no
     *         es STRUCT/CLASS (caller debe emitir error).
     *
     * Para CLASS los offsets incluyen los fields heredados (mismo
     * convenio que `ClassLayout::fields`).  Bit fields devuelven el
     * offset del storage word (no del bit; usar otra API si se necesita
     * la posicion exacta del bit).
     */
    int64_t comptime_field_offset(const TypeChecker &tc, const Type &t,
                                   const std::string &field_name);

    /**
     * @brief @c true si @c t tiene un campo llamado @c name (struct/class).
     */
    bool comptime_has_field(const TypeChecker &tc, const Type &t,
                             const std::string &name);

    /**
     * @brief @c true si @c t tiene un metodo llamado @c name (solo CLASS).
     *
     * Para STRUCT siempre devuelve @c false (no soporta metodos virtuales).
     */
    bool comptime_has_method(const TypeChecker &tc, const Type &t,
                              const std::string &name);

    /**
     * @brief Numero de fields declarados en @c t.
     *
     * Para CLASS incluye heredados.  Para STRUCT cuenta todos los campos.
     * Para ENUM cuenta variantes (no payload fields).  Para tipos sin
     * fields (primitivos, ptr, etc.) devuelve 0.
     */
    uint32_t comptime_field_count(const TypeChecker &tc, const Type &t);

    /**
     * @brief Numero de metodos declarados en @c t (solo CLASS).
     *
     * Incluye heredados.  Para tipos no-CLASS devuelve 0.
     */
    uint32_t comptime_method_count(const TypeChecker &tc, const Type &t);

    /**
     * @brief Nombre del campo @c idx-esimo en @c t (orden de declaracion).
     *
     * Devuelve cadena vacia si @c idx es invalido o @c t no soporta campos.
     */
    std::string comptime_field_name(const TypeChecker &tc, const Type &t,
                                     uint32_t idx);

    /**
     * @brief Nombre canonico del tipo del campo @c field_name en @c t.
     *
     * Devuelve cadena vacia si el campo no existe.  Equivalente a
     * `typename<typeof(t.field)>()` pero sin requerir sintaxis especial.
     */
    std::string comptime_field_type_name(const TypeChecker &tc, const Type &t,
                                          const std::string &field_name);

    /**
     * @brief evalua una expresion AST en compile-time.
     *
     * Soporta literales (bool/int), builtins comptime (sizeof, alignof,
     * type_id, kind, field_count, method_count, offsetof + predicates
     * has_field/has_method/is_*) y operadores aritmeticos, logicos,
     * bitwise y de comparacion.
     * NO soporta llamadas a funciones de usuario, ni accesos a variables
     * runtime (estos siempre devuelven ok=false).
     *
     * Devuelve un par (ok, value):  ok=true si la expresion es
     * comptime-evaluable; value contiene el resultado como int64_t
     * (booleanos se codifican como 0/1).  Si ok=false, la expresion no
     * es resoluble en compile-time y el caller debe emitir error.
     */
    /**
     * @brief resultado de evaluacion comptime extendido.
     *
     * Puede contener un entero (default) o un string.  El flag `is_str`
     * distingue.  El campo `ok=false` indica que la expresion no es
     * comptime-evaluable -- el contenido es indeterminado.
     */
    /* `ComptimeValue` esta declarado en `type_checker.h` (antes que
     * `ComptimeConst`) para poder ser usado en su layout.  Lo reusamos
     * aqui para los arrays/structs anidados de `ComptimeEvalResult`. */

    struct ComptimeEvalResult {
        bool        ok        = false;
        bool        is_str    = false;
        bool        is_array  = false;
        bool        is_struct = false;
        bool        is_type   = false;
        int64_t     value     = 0;
        std::string str;
        std::vector<std::shared_ptr<ComptimeValue>> array_vals;
        std::unordered_map<std::string, std::shared_ptr<ComptimeValue>> struct_fields;
        Type        type_val;
    };

    /// Conversion entre Result y Value.  Result = Value + ok flag.
    inline ComptimeValue value_from_result(const ComptimeEvalResult &r) {
        ComptimeValue v;
        v.is_str        = r.is_str;
        v.is_array      = r.is_array;
        v.is_struct     = r.is_struct;
        v.is_type       = r.is_type;
        v.value         = r.value;
        v.str           = r.str;
        v.array_vals    = r.array_vals;
        v.struct_fields = r.struct_fields;
        v.type_val      = r.type_val;
        return v;
    }
    inline ComptimeEvalResult result_from_value(const ComptimeValue &v) {
        ComptimeEvalResult r;
        r.ok            = true;
        r.is_str        = v.is_str;
        r.is_array      = v.is_array;
        r.is_struct     = v.is_struct;
        r.is_type       = v.is_type;
        r.value         = v.value;
        r.str           = v.str;
        r.array_vals    = v.array_vals;
        r.struct_fields = v.struct_fields;
        r.type_val      = v.type_val;
        return r;
    }
    ComptimeEvalResult comptime_eval_expr(const TypeChecker &tc,
                                           const ast::Expr *expr);

    /**
     * @brief estado de control de flujo para el interprete
     * comptime.  Usado por @c comptime_eval_stmt para propagar @c return,
     * @c break y @c continue dentro de bodies de funciones / loops
     * comptime sin necesidad de excepciones.
     */
    struct ComptimeControl {
        bool returned       = false;
        bool break_seen     = false;
        bool continue_seen  = false;
        ComptimeEvalResult return_value;
    };

    /**
     * @brief ejecuta un Stmt en compile-time.
     *
     * Mutable scope stack en @c tc.comptime_const_locals_.  Soporta:
     * BlockStmt, VarDeclStmt (registra binding), IfStmt (rama segun
     * cond comptime), ReturnStmt, BreakStmt, ContinueStmt, WhileStmt,
     * DoWhileStmt, ForStmt, y ExprStmt con AssignExpr a comptime var
     * (actualiza binding).  Devuelve false si la ejecucion fallo (e.g.
     * expression no comptime-evaluable, asignacion a const, loop excedio
     * limite de iteraciones).
     */
    bool comptime_eval_stmt(TypeChecker &tc,
                             const ast::Stmt *s,
                             ComptimeControl &ctrl);

    /**
     * @brief Tipo semantico del campo @c field_name en @c t.
     *
     * Devuelve un @c Type con `kind=COUNT` (sentinela "no encontrado") si
     * el campo no existe o @c t no soporta campos.  Para Sprint 3 (field_get/
     * field_set) sirve para resolver el tipo de retorno y validar
     * compatibilidad del valor a almacenar sin pasar por strings.
     */
    Type comptime_field_type(const TypeChecker &tc, const Type &t,
                              const std::string &field_name);

    // -------------------------------------------------------------------
    // Sprint 2: queries de tipos (compile-time).
    // -------------------------------------------------------------------

    /**
     * @brief @c true si @c sub es subtipo de @c super.
     *
     * Reusa @c TypeChecker::class_is_assignable que recorre la jerarquia
     * (super + interfaces transitivas).  Reflexivo: `is_subtype<T,T>() == true`.
     */
    bool comptime_is_subtype(const TypeChecker &tc, const Type &sub,
                              const Type &super);

    /**
     * @brief @c true si @c a y @c b son el mismo tipo logico.
     *
     * Compara los nombres canonicos (@c comptime_type_name); equivalente
     * a `type_id<A>() == type_id<B>()` pero sin la indireccion del hash.
     */
    bool comptime_is_same(const TypeChecker &tc, const Type &a,
                           const Type &b);

    /**
     * @brief @c true si @c t es un tipo CLASS (reference type).
     */
    bool comptime_is_class(const Type &t);

    /**
     * @brief @c true si @c t es un STRUCT puro (no enum ni class).
     *
     * Distingue STRUCT real de ENUM (que tambien usa PrimitiveKind::STRUCT
     * con su `struct_name` en `enum_layouts_`).
     */
    bool comptime_is_struct(const TypeChecker &tc, const Type &t);

    /**
     * @brief @c true si @c t es un primitivo escalar (i*, u*, f*, bool, char, void).
     *
     * Excluye STRING, PTR, ARRAY, FUNCTION, OPTIONAL, RESULT y wrappers
     * (UNIQUE/SHARED/BORROW/FUTURE) -- todos esos tienen sus kinds propios.
     */
    bool comptime_is_primitive(const Type &t);

    /**
     * @brief @c true si @c t es un newtype (typedef T name new).
     *
     * Devuelve true para @c "typedef u64 port new;" -> @c port.  Cualquier
     * tipo cuyo @c nominal_id sea distinto de 0.
     */
    bool comptime_is_newtype(const Type &t);

    /**
     * @brief @c true si @c t es un newtype @c @opaque.
     *
     * Implica @c comptime_is_newtype(t) == true.
     */
    bool comptime_is_opaque(const Type &t);

    /**
     * @brief Devuelve el nombre canonico del underlying de un newtype.
     *
     * Si @c t no es un newtype, devuelve el typename habitual (sin
     * indirection).  Si es un newtype, recupera el typename del tipo
     * subyacente (e.g. @c "u64" para @c "typedef u64 port new").
     */
    std::string comptime_underlying_name(const TypeChecker &tc, const Type &t);

} // namespace vex

#endif // VEX_COMPTIME_INTROSPECT_H
