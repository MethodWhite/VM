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

namespace vex {

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

} // namespace vex
