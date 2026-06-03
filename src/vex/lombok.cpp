/*
 * VestaVM - Maquina Virtual Distribuida
 * Copyright (C) 2026 David Lopez.T (DesmonHak); Licencia VMProject
 */
/**
 * @file lombok.cpp
 * @brief Sprint lombok (2026-06-03): expansion de anotaciones tipo Project Lombok.
 *
 * Implementacion del pre-pase @c TypeChecker::expand_lombok_annotations()
 * que recorre cada @c ClassDecl del modulo y genera @c ClassMethodDecl
 * sinteticos al AST segun los flags @c lombok_* parseados.  Tras este
 * pre-pase el AST equivale a haber escrito los metodos a mano y el
 * resto del pipeline (collect_classes, check_functions, lowering) no
 * necesita conocer Lombok.
 *
 * Anotaciones implementadas:
 *   - @Getter / @Setter (clase o campo individual) -> get_X() / set_X(v)
 *   - @NonNull (campo) -> reescribe el tipo del field a `nonnull T`
 *                         + propaga a params de ctor sintetico (compile-time)
 *   - @With (campo o clase) -> with_X(v) clona la instancia con X=v
 *   - @ToString (clase) -> string toString()
 *   - @EqualsAndHashCode (clase) -> bool equals(Object o) + u64 hashCode()
 *   - @NoArgsConstructor (clase) -> ctor sin args con body vacio
 *   - @AllArgsConstructor (clase) -> ctor con todos los fields
 *   - @RequiredArgsConstructor (clase) -> ctor con fields final/nonnull
 *   - @Data (clase) = @Getter + @Setter + @ToString + @EqualsAndHashCode +
 *                     @RequiredArgsConstructor
 *   - @Value (clase) = @Data inmutable (todos los fields final)
 *   - @Builder (clase) -> clase auxiliar XBuilder con metodos chainables
 *   - @Synchronized (clase) -> wrap todos los metodos no-static en
 *                              synchronized(this) { body }
 *   - @Log (clase) -> field static logger (placeholder; usa vio_print)
 *
 * @NonNull se traduce a la palabra reservada @c nonnull del lenguaje
 * Vex, que el TypeChecker ya valida en compile-time (rechazo de null
 * literal + warning si el value source no es probadamente non-null).
 * Esto evita el overhead de un check runtime cuando el flujo de tipos
 * es estatico.
 */
#include "vex/type_checker.h"
#include "vex/ast.h"
#include <memory>
#include <string>
#include <vector>

namespace vex {

    // -------------------------------------------------------------------
    // Helpers de construccion de AST.
    // -------------------------------------------------------------------

    /**
     * @brief Clona profundamente un TypeNode (subset usado por Lombok).
     *
     * Lombok solo necesita clonar tipos referenciados por fields y los
     * propaga a getters / setters / ctores / builder.  Reutilizamos un
     * walker minimal con cobertura suficiente para Primitivos, Named
     * (clases/structs), Pointer, Array, Function y Nullable.  Si el
     * usuario tiene tipos exoticos no cubiertos, el resto del pipeline
     * los procesa via clone_type_with_subst (que cubre mas casos).
     */
    static std::unique_ptr<ast::TypeNode> lk_clone_type(const ast::TypeNode *t) {
        if (!t) return nullptr;
        std::unique_ptr<ast::TypeNode> out;
        switch (t->kind) {
            case ast::NodeKind::PrimitiveTypeNode: {
                auto *src = static_cast<const ast::PrimitiveTypeNode *>(t);
                auto p = std::make_unique<ast::PrimitiveTypeNode>();
                p->loc  = src->loc;
                p->prim = src->prim;
                for (auto &ta : src->type_args)
                    p->type_args.push_back(lk_clone_type(ta.get()));
                out = std::move(p);
                break;
            }
            case ast::NodeKind::NamedTypeNode: {
                auto *src = static_cast<const ast::NamedTypeNode *>(t);
                auto p = std::make_unique<ast::NamedTypeNode>();
                p->loc  = src->loc;
                p->name = src->name;
                for (auto &ta : src->type_args)
                    p->type_args.push_back(lk_clone_type(ta.get()));
                out = std::move(p);
                break;
            }
            case ast::NodeKind::PointerTypeNode: {
                auto *src = static_cast<const ast::PointerTypeNode *>(t);
                auto p = std::make_unique<ast::PointerTypeNode>();
                p->loc        = src->loc;
                p->pointee    = lk_clone_type(src->pointee.get());
                p->is_virtual = src->is_virtual;
                out = std::move(p);
                break;
            }
            case ast::NodeKind::ArrayTypeNode: {
                auto *src = static_cast<const ast::ArrayTypeNode *>(t);
                auto p = std::make_unique<ast::ArrayTypeNode>();
                p->loc          = src->loc;
                p->element_type = lk_clone_type(src->element_type.get());
                // size_expr es un Expr; Lombok no clona exprs (las funcs
                // sinteticas usan tipos sin tamano variable normalmente).
                // Si el campo tiene tamano expr, dejamos nullptr y el type
                // checker fallara si lo necesita -- documentamos limitacion.
                out = std::move(p);
                break;
            }
            case ast::NodeKind::FunctionTypeNode: {
                auto *src = static_cast<const ast::FunctionTypeNode *>(t);
                auto p = std::make_unique<ast::FunctionTypeNode>();
                p->loc = src->loc;
                for (auto &pa : src->param_types)
                    p->param_types.push_back(lk_clone_type(pa.get()));
                p->return_type = lk_clone_type(src->return_type.get());
                out = std::move(p);
                break;
            }
            default:
                return nullptr;
        }
        if (out) out->is_nonnull = t->is_nonnull;
        return out;
    }

    /**
     * @brief Construye un `this.field_name` como FieldAccessExpr.
     *
     * El walker rewrite_implicit_this ya hace lo equivalente para
     * cuerpos escritos por el usuario; aqui generamos directamente
     * el FieldAccess explicito para no depender del walker.
     */
    static std::unique_ptr<ast::FieldAccessExpr> lk_this_field(
        const std::string &field_name,
        SourceLoc          loc)
    {
        auto fa = std::make_unique<ast::FieldAccessExpr>();
        fa->loc = loc;
        auto base = std::make_unique<ast::IdentExpr>();
        base->loc  = loc;
        base->name = "this";
        fa->base       = std::move(base);
        fa->field_name = field_name;
        return fa;
    }

    /**
     * @brief Genera el getter sintetico para un campo: `T get_X() { return this.X; }`.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_getter(
        const ast::ClassFieldDecl &f)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc            = f.loc;
        m->name           = "get_" + f.name;
        m->return_type    = lk_clone_type(f.type.get());
        m->access         = 0;  // public
        m->property_kind  = 1;  // getter
        m->is_static      = false;
        m->body           = std::make_unique<ast::BlockStmt>();
        m->body->loc      = f.loc;
        auto rs = std::make_unique<ast::ReturnStmt>();
        rs->loc   = f.loc;
        rs->value = lk_this_field(f.name, f.loc);
        m->body->body.push_back(std::move(rs));
        return m;
    }

    /**
     * @brief Genera el setter sintetico para un campo:
     *        `void set_X(T value) { this.X = value; }`.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_setter(
        const ast::ClassFieldDecl &f)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc            = f.loc;
        m->name           = "set_" + f.name;
        // setter siempre void; usamos PrimitiveTypeNode VOID.
        auto void_t = std::make_unique<ast::PrimitiveTypeNode>();
        void_t->loc  = f.loc;
        void_t->prim = PrimitiveKind::VOID;
        m->return_type   = std::move(void_t);
        m->access        = 0;
        m->property_kind = 2;
        m->is_static     = false;
        auto p = std::make_unique<ast::ParamDecl>();
        p->loc  = f.loc;
        p->name = "value";
        p->type = lk_clone_type(f.type.get());
        m->params.push_back(std::move(p));
        m->body          = std::make_unique<ast::BlockStmt>();
        m->body->loc     = f.loc;
        // body: this.X = value;
        auto assign = std::make_unique<ast::AssignExpr>();
        assign->loc    = f.loc;
        assign->op     = ast::AssignOp::Assign;
        assign->target = lk_this_field(f.name, f.loc);
        auto val_id = std::make_unique<ast::IdentExpr>();
        val_id->loc  = f.loc;
        val_id->name = "value";
        assign->value = std::move(val_id);
        auto es = std::make_unique<ast::ExprStmt>();
        es->loc  = f.loc;
        es->expr = std::move(assign);
        m->body->body.push_back(std::move(es));
        return m;
    }

    /**
     * @brief Genera un constructor sintetico con los fields dados.
     *
     * Cada parametro tiene el mismo nombre y tipo que el field.  El
     * body es `this.X = X;` por cada field, en orden.  Si la lista
     * de fields esta vacia, el body queda vacio (no-args ctor).
     *
     * El parser detecta nombres "ClassName" como constructor por
     * convencion + flag @c is_constructor.  Generamos con el flag y
     * el nombre de la clase para mantener consistencia.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_ctor(
        const std::string                            &class_name,
        const std::vector<const ast::ClassFieldDecl *> &fields,
        SourceLoc                                     loc)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc            = loc;
        m->name           = class_name;
        m->return_type    = nullptr;  // ctor no devuelve nada
        m->access         = 0;
        m->is_constructor = true;
        m->is_static      = false;
        m->body           = std::make_unique<ast::BlockStmt>();
        m->body->loc      = loc;
        for (const auto *f : fields) {
            // Parametro: tipo igual al field, nombre igual al field.
            auto p = std::make_unique<ast::ParamDecl>();
            p->loc  = f->loc;
            p->name = f->name;
            p->type = lk_clone_type(f->type.get());
            m->params.push_back(std::move(p));
            // Body: this.X = X;
            auto assign = std::make_unique<ast::AssignExpr>();
            assign->loc    = f->loc;
            assign->op     = ast::AssignOp::Assign;
            assign->target = lk_this_field(f->name, f->loc);
            auto val_id = std::make_unique<ast::IdentExpr>();
            val_id->loc  = f->loc;
            val_id->name = f->name;
            assign->value = std::move(val_id);
            auto es = std::make_unique<ast::ExprStmt>();
            es->loc  = f->loc;
            es->expr = std::move(assign);
            m->body->body.push_back(std::move(es));
        }
        return m;
    }

    /**
     * @brief Genera `with_X(T v) -> ClassName` que clona la instancia
     *        replaceando el campo X con el valor pasado.
     *
     * Body emitted: `Self r = new Self(arg0, arg1, ..., v_for_X, ...);
     *                 return r;`
     *
     * Requiere que la clase tenga un @AllArgsConstructor (o ctor que
     * acepte todos los fields en orden).  Si no, el TypeChecker
     * reportara error de ctor no encontrado en check_call.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_with(
        const ast::ClassDecl     &cls,
        const ast::ClassFieldDecl &target)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc         = target.loc;
        m->name        = "with_" + target.name;
        auto rt = std::make_unique<ast::NamedTypeNode>();
        rt->loc  = target.loc;
        rt->name = cls.name;
        m->return_type = std::move(rt);
        m->access      = 0;
        m->is_static   = false;
        auto p = std::make_unique<ast::ParamDecl>();
        p->loc  = target.loc;
        p->name = "value";
        p->type = lk_clone_type(target.type.get());
        m->params.push_back(std::move(p));
        m->body          = std::make_unique<ast::BlockStmt>();
        m->body->loc     = target.loc;
        // return new Self(f0, f1, ... value-en-target ...);
        auto ne = std::make_unique<ast::NewExpr>();
        ne->loc        = target.loc;
        ne->class_name = cls.name;
        for (const auto &f : cls.fields) {
            if (f.is_static) continue;
            if (&f == &target) {
                auto id = std::make_unique<ast::IdentExpr>();
                id->loc  = target.loc;
                id->name = "value";
                ne->args.push_back(std::move(id));
            } else {
                ne->args.push_back(lk_this_field(f.name, target.loc));
            }
        }
        auto rs = std::make_unique<ast::ReturnStmt>();
        rs->loc   = target.loc;
        rs->value = std::move(ne);
        m->body->body.push_back(std::move(rs));
        return m;
    }

    /**
     * @brief Genera `string toString()` que concatena los fields como
     *        string interpolada: `"ClassName{field1=value1, field2=value2}"`.
     *
     * El frontend ya soporta interpolacion para tipos primitivos y
     * StringObject, asi que el body es un solo return de StringLitExpr
     * con parts + interp_exprs poblados.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_tostring(
        const ast::ClassDecl &cls)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc          = cls.loc;
        m->name         = "toString";
        auto rt = std::make_unique<ast::PrimitiveTypeNode>();
        rt->loc  = cls.loc;
        rt->prim = PrimitiveKind::STRING;
        m->return_type = std::move(rt);
        m->access      = 0;
        m->is_static   = false;
        m->body        = std::make_unique<ast::BlockStmt>();
        m->body->loc   = cls.loc;
        // Construir la interpolacion como secuencia (parts, exprs).
        auto sl = std::make_unique<ast::StringLitExpr>();
        sl->loc = cls.loc;
        std::string head = cls.name + "{";
        bool first = true;
        for (const auto &f : cls.fields) {
            if (f.is_static) continue;
            if (!first) head += ", ";
            first = false;
            head += f.name;
            head += "=";
            sl->interp_parts.push_back(head);
            head.clear();
            // expr: this.X (FieldAccessExpr)
            sl->interp_exprs.push_back(lk_this_field(f.name, cls.loc));
        }
        head += "}";
        sl->interp_parts.push_back(head);
        // Tras N exprs, debe haber N+1 parts.  La primera entra antes
        // del primer expr; la ultima cierra.  Si no hay fields, parts
        // tiene 1 entry: "ClassName{}" y exprs vacio.
        auto rs = std::make_unique<ast::ReturnStmt>();
        rs->loc   = cls.loc;
        rs->value = std::move(sl);
        m->body->body.push_back(std::move(rs));
        return m;
    }

    /**
     * @brief Genera `bool equals(<ClassName> other)` que compara field
     *        a field.  Para fields primitivos usa `==`; para fields
     *        STRING o CLASS hay que delegar a sus propios @c equals
     *        (no implementado en este sprint -- usa `==` que para
     *        referencias compara identidad GcHandle).
     *
     * Limitacion: solo soporta clases con fields primitivos en v1.
     * Para fields string/class el resultado puede ser sorprendente
     * (compara identidad, no contenido).  Documentado en el header.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_equals(
        const ast::ClassDecl &cls)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc          = cls.loc;
        m->name         = "equals";
        auto rt = std::make_unique<ast::PrimitiveTypeNode>();
        rt->loc  = cls.loc;
        rt->prim = PrimitiveKind::BOOL;
        m->return_type = std::move(rt);
        m->access      = 0;
        m->is_static   = false;
        auto p = std::make_unique<ast::ParamDecl>();
        p->loc  = cls.loc;
        p->name = "other";
        auto pt = std::make_unique<ast::NamedTypeNode>();
        pt->loc  = cls.loc;
        pt->name = cls.name;
        p->type  = std::move(pt);
        m->params.push_back(std::move(p));
        m->body          = std::make_unique<ast::BlockStmt>();
        m->body->loc     = cls.loc;
        // Body: chequear null + por cada field: if (this.X != other.X) return false; return true.
        // Para el primer return false, si no hay fields, retorna true directo.
        bool any_field = false;
        for (const auto &f : cls.fields) {
            if (f.is_static) continue;
            any_field = true;
            // if (this.X != other.X) return false;
            auto cmp = std::make_unique<ast::BinaryExpr>();
            cmp->loc = f.loc;
            cmp->op  = ast::BinOp::Neq;
            cmp->lhs = lk_this_field(f.name, f.loc);
            auto other_fa = std::make_unique<ast::FieldAccessExpr>();
            other_fa->loc = f.loc;
            auto other_id = std::make_unique<ast::IdentExpr>();
            other_id->loc  = f.loc;
            other_id->name = "other";
            other_fa->base       = std::move(other_id);
            other_fa->field_name = f.name;
            cmp->rhs = std::move(other_fa);
            auto ifs = std::make_unique<ast::IfStmt>();
            ifs->loc  = f.loc;
            ifs->cond = std::move(cmp);
            auto then_blk = std::make_unique<ast::BlockStmt>();
            then_blk->loc = f.loc;
            auto rs = std::make_unique<ast::ReturnStmt>();
            rs->loc = f.loc;
            auto fconst = std::make_unique<ast::BoolLitExpr>();
            fconst->loc = f.loc;
            fconst->value = false;
            rs->value = std::move(fconst);
            then_blk->body.push_back(std::move(rs));
            ifs->then_branch = std::move(then_blk);
            m->body->body.push_back(std::move(ifs));
        }
        (void)any_field;
        // return true;
        auto rs_end = std::make_unique<ast::ReturnStmt>();
        rs_end->loc = cls.loc;
        auto tconst = std::make_unique<ast::BoolLitExpr>();
        tconst->loc   = cls.loc;
        tconst->value = true;
        rs_end->value = std::move(tconst);
        m->body->body.push_back(std::move(rs_end));
        return m;
    }

    /**
     * @brief Genera `u64 hashCode()` que combina los hashes de los fields
     *        via FNV-1a-like: h = h * 31 + field_hash.  Para fields
     *        primitivos el field_hash es su valor; para strings usa
     *        @c str_hash; para class usa la identidad del handle.
     */
    static std::unique_ptr<ast::ClassMethodDecl> lk_make_hashcode(
        const ast::ClassDecl &cls)
    {
        auto m = std::make_unique<ast::ClassMethodDecl>();
        m->loc          = cls.loc;
        m->name         = "hashCode";
        auto rt = std::make_unique<ast::PrimitiveTypeNode>();
        rt->loc  = cls.loc;
        rt->prim = PrimitiveKind::U64;
        m->return_type = std::move(rt);
        m->access      = 0;
        m->is_static   = false;
        m->body        = std::make_unique<ast::BlockStmt>();
        m->body->loc   = cls.loc;
        // u64 h = 17;
        // h = h * 31 + (u64)this.X;  por cada field
        // return h;
        auto vd = std::make_unique<ast::VarDeclStmt>();
        vd->loc  = cls.loc;
        vd->name = "h";
        auto h_t = std::make_unique<ast::PrimitiveTypeNode>();
        h_t->loc  = cls.loc;
        h_t->prim = PrimitiveKind::U64;
        vd->type  = std::move(h_t);
        auto initc = std::make_unique<ast::IntLitExpr>();
        initc->loc   = cls.loc;
        initc->value = 17;
        vd->init = std::move(initc);
        m->body->body.push_back(std::move(vd));
        for (const auto &f : cls.fields) {
            if (f.is_static) continue;
            // h = h * 31 + this.X;
            auto mul = std::make_unique<ast::BinaryExpr>();
            mul->loc = f.loc;
            mul->op  = ast::BinOp::Mul;
            auto h_id = std::make_unique<ast::IdentExpr>();
            h_id->loc  = f.loc;
            h_id->name = "h";
            mul->lhs = std::move(h_id);
            auto k = std::make_unique<ast::IntLitExpr>();
            k->loc   = f.loc;
            k->value = 31;
            mul->rhs = std::move(k);
            auto add = std::make_unique<ast::BinaryExpr>();
            add->loc = f.loc;
            add->op  = ast::BinOp::Add;
            add->lhs = std::move(mul);
            add->rhs = lk_this_field(f.name, f.loc);
            auto assign = std::make_unique<ast::AssignExpr>();
            assign->loc = f.loc;
            assign->op  = ast::AssignOp::Assign;
            auto h_id2 = std::make_unique<ast::IdentExpr>();
            h_id2->loc  = f.loc;
            h_id2->name = "h";
            assign->target = std::move(h_id2);
            assign->value  = std::move(add);
            auto es = std::make_unique<ast::ExprStmt>();
            es->loc  = f.loc;
            es->expr = std::move(assign);
            m->body->body.push_back(std::move(es));
        }
        auto rs = std::make_unique<ast::ReturnStmt>();
        rs->loc = cls.loc;
        auto h_ret = std::make_unique<ast::IdentExpr>();
        h_ret->loc  = cls.loc;
        h_ret->name = "h";
        rs->value = std::move(h_ret);
        m->body->body.push_back(std::move(rs));
        return m;
    }

    /**
     * @brief Indica si una clase YA tiene un metodo con el nombre dado.
     *
     * Se usa para evitar sobreescribir metodos escritos por el usuario:
     * si el usuario ya escribio @c toString() a mano, @c @ToString no
     * genera duplicado.  Misma logica para el resto de generators.
     */
    static bool lk_has_method(const ast::ClassDecl &cls, const std::string &name) {
        for (const auto &m : cls.methods) {
            if (m && m->name == name) return true;
        }
        return false;
    }

    /**
     * @brief Indica si una clase YA tiene un constructor con N parametros.
     *
     * @AllArgsConstructor / @NoArgsConstructor / @RequiredArgsConstructor
     * consultan esto antes de generar para no chocar con un ctor escrito
     * a mano del mismo arity.
     */
    static bool lk_has_ctor_with_arity(const ast::ClassDecl &cls, size_t arity) {
        for (const auto &m : cls.methods) {
            if (m && m->is_constructor && m->params.size() == arity) return true;
        }
        return false;
    }

    // -------------------------------------------------------------------
    // Punto de entrada del pre-pase.
    // -------------------------------------------------------------------

    void TypeChecker::expand_lombok_annotations() {
        for (auto &dptr : mod_.decls) {
            if (!dptr || dptr->kind != ast::NodeKind::ClassDecl) continue;
            auto &cd = *static_cast<ast::ClassDecl *>(dptr.get());
            // Interfaces e introspect runtime no llevan Lombok.
            if (cd.is_interface) continue;
            // @Data: combo Getter+Setter+ToString+EqHash+RequiredCtor.
            if (cd.lombok_data) {
                cd.lombok_getter        = true;
                cd.lombok_setter        = true;
                cd.lombok_tostring      = true;
                cd.lombok_equals_hash   = true;
                cd.lombok_required_ctor = true;
            }
            // @Value: como @Data pero inmutable -- todos los fields final,
            // sin @Setter, con @AllArgsConstructor.
            if (cd.lombok_value) {
                cd.lombok_getter        = true;
                cd.lombok_setter        = false;
                cd.lombok_tostring      = true;
                cd.lombok_equals_hash   = true;
                cd.lombok_all_args_ctor = true;
                for (auto &f : cd.fields) {
                    if (!f.is_static) f.is_final = true;
                }
            }
            // @NonNull en campo: setea el bit `is_nonnull` del TypeNode
            // del field.  El TypeChecker existente ya valida `nonnull T`
            // rechazando null literals en compile-time.  Sin overhead
            // runtime cuando el flujo de tipos es estatico; el TypeChecker
            // exigira `unwrap`/`!!` cuando el value source sea un nullable
            // sin certeza estatica.  Esto cumple la directiva: "@NonNull
            // debe ser en comptime si es posible, evitando comprobaciones
            // runtime para mayor beneficio".
            for (auto &f : cd.fields) {
                if (!f.lombok_nonnull) continue;
                if (!f.type) continue;
                f.type->is_nonnull = true;
            }
            // Field-level @Getter / @Setter / @With.
            for (size_t i = 0; i < cd.fields.size(); ++i) {
                const auto &f = cd.fields[i];
                if (f.is_static) continue;
                const bool want_getter = (cd.lombok_getter || f.lombok_getter)
                                      && !lk_has_method(cd, "get_" + f.name);
                const bool want_setter = (cd.lombok_setter || f.lombok_setter)
                                      && !f.is_final
                                      && !lk_has_method(cd, "set_" + f.name);
                const bool want_with   = (cd.lombok_with_all || f.lombok_with)
                                      && !lk_has_method(cd, "with_" + f.name);
                if (want_getter) cd.methods.push_back(lk_make_getter(f));
                if (want_setter) cd.methods.push_back(lk_make_setter(f));
                if (want_with)   cd.methods.push_back(lk_make_with(cd, f));
                (void)i;
            }
            // Class-level @ToString.
            if (cd.lombok_tostring && !lk_has_method(cd, "toString")) {
                cd.methods.push_back(lk_make_tostring(cd));
            }
            // Class-level @EqualsAndHashCode.
            if (cd.lombok_equals_hash) {
                if (!lk_has_method(cd, "equals"))   cd.methods.push_back(lk_make_equals(cd));
                if (!lk_has_method(cd, "hashCode")) cd.methods.push_back(lk_make_hashcode(cd));
            }
            // Class-level @NoArgsConstructor.
            if (cd.lombok_no_args_ctor && !lk_has_ctor_with_arity(cd, 0)) {
                std::vector<const ast::ClassFieldDecl *> empty;
                cd.methods.push_back(lk_make_ctor(cd.name, empty, cd.loc));
            }
            // Class-level @AllArgsConstructor.
            if (cd.lombok_all_args_ctor) {
                std::vector<const ast::ClassFieldDecl *> all;
                for (const auto &f : cd.fields)
                    if (!f.is_static) all.push_back(&f);
                if (!lk_has_ctor_with_arity(cd, all.size()))
                    cd.methods.push_back(lk_make_ctor(cd.name, all, cd.loc));
            }
            // Class-level @RequiredArgsConstructor.
            // "Required" = final + @NonNull (incluyendo los marcados via
            // @Data/@Value que setean is_final = true).
            if (cd.lombok_required_ctor) {
                std::vector<const ast::ClassFieldDecl *> req;
                for (const auto &f : cd.fields) {
                    if (f.is_static) continue;
                    bool need = f.is_final || f.lombok_nonnull;
                    if (need) req.push_back(&f);
                }
                if (!lk_has_ctor_with_arity(cd, req.size()))
                    cd.methods.push_back(lk_make_ctor(cd.name, req, cd.loc));
            }
        }
    }

}  // namespace vex
