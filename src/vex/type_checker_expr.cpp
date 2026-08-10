#include "vex/lowering.h"
#include "vex/type_checker.h"
#include "vex/ast.h"
#include "vex/diagnostic.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>
#include <functional>
#include "vex/comptime_introspect.h"
#include "vex/collection_intrinsics.h"
#include "vex/lexer.h"
#include "vex/parser.h"

// Forward decl: defined in type_checker.cpp
namespace vex {
std::string mangle_args(const std::vector<Type> &args);
}

namespace vex {

    /// Nombre del metodo dunder (__op__) para un operador binario, o vacio.
    static const char *dunder_method_for_binop(ast::BinOp op) {
        switch (op) {
            case ast::BinOp::Add:          return "__add__";
            case ast::BinOp::Sub:          return "__sub__";
            case ast::BinOp::Mul:          return "__mul__";
            case ast::BinOp::Div:          return "__div__";
            case ast::BinOp::Mod:          return "__mod__";
            case ast::BinOp::Eq:           return "__eq__";
            case ast::BinOp::Neq:          return "__ne__";
            case ast::BinOp::Lt:           return "__lt__";
            case ast::BinOp::Le:           return "__le__";
            case ast::BinOp::Gt:           return "__gt__";
            case ast::BinOp::Ge:           return "__ge__";
            case ast::BinOp::BitAnd:       return "__and__";
            case ast::BinOp::BitOr:        return "__or__";
            case ast::BinOp::BitXor:       return "__xor__";
            case ast::BinOp::Shl:          return "__shl__";
            case ast::BinOp::Shr:          return "__shr__";
            default:                       return nullptr;
        }
    }

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

        /* Operator overloading via metodos dunder (C-1).  Si el lhs es una
         * CLASS/STRUCT que declara el metodo __op__ aceptando el tipo del
         * rhs, se marca e->overload_method para que el lowering despache a
         * lhs.__op__(rhs) en vez de la aritmetica clasica.  Sin el dunder,
         * el comportamiento clasico queda intacto. */
        if (tl.kind == PrimitiveKind::CLASS || tl.kind == PrimitiveKind::STRUCT) {
            const std::string dunder = dunder_method_for_binop(e->op);
            if (!dunder.empty() && !tl.struct_name.empty()) {
                /* Solo las CLASS tienen metodos en esta version (los structs
                 * son POD).  El dunder de structs es un port posterior de
                 * Desmon (StructLayout con metodos). */
                auto itc = class_layouts_.find(tl.struct_name);
                if (itc != class_layouts_.end()) {
                    for (const auto &m : itc->second.methods) {
                        if (m.name == dunder) {
                            e->overload_method = dunder;
                            return m.return_type;
                        }
                    }
                }
            }
        }

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
