/**
 * @file namespace_flatten.cpp
 * @brief Pre-pass que aplana @c NamespaceDecl inline (Phase M.7.c).
 *
 * Transforma el AST tras parsear y antes del TypeChecker:
 *   1. Encuentra todos los @c NamespaceDecl top-level (y anidados).
 *   2. Para cada uno, aplica el prefijo de mangling a todas las
 *      declaraciones internas (FunctionDecl, ClassDecl, StructDecl,
 *      EnumDecl, TypeAliasDecl, GlobalVarDecl).  Anidamiento concatena:
 *      `namespace a { namespace b { class C } }` -> @c a__b__C.
 *   3. Reescribe REFERENCIAS internas (IdentExpr, NewExpr::class_name,
 *      NamedTypeNode, etc.) a los nombres mangled.
 *   4. "Sube" los decls extraidos al top-level del Module, descartando
 *      el @c NamespaceDecl wrapper.
 *   5. Devuelve un descriptor @c FlattenedNamespace por cada namespace
 *      encontrado, para que el caller los registre como
 *      @c Symbol::Namespace en el TypeChecker (reusa la infra de M7.a).
 *
 * Cero overhead runtime: el namespace es puramente lexical.  Los labels
 * mangled (`ui__Button`, `audio__Button`) son identificadores C validos,
 * compatibles con port-c y el linker actual del .velb.
 */

#include "vex/namespace_flatten.h"

#include <functional>
#include <unordered_map>

#include "vex/ast.h"

namespace vex {

namespace {

/// Aplica el prefix `<ns_path>__` a un nombre si NO empieza con `__`
/// (identificadores reservados) y no es `main` (entry point unico).
std::string mangle_name_(const std::string &ns_path, const std::string &name) {
    if (name.empty()) return name;
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_') return name;
    if (name == "main") return name;
    return ns_path + "__" + name;
}

/// Walker recursivo que reescribe IdentExpr / NamedTypeNode / NewExpr
/// que matchean nombres del @p rename_map.  Mismo patron que
/// @c mangle_top_level_ de @c compiler_project.cpp pero re-uso aqui
/// para mantener namespace_flatten autocontenido (compile_project usa
/// este modulo para los deps con namespaces tambien).
void rewrite_refs_in_expr_(ast::Expr *e,
                            const std::unordered_map<std::string, std::string> &rename_map);
void rewrite_refs_in_stmt_(ast::Stmt *s,
                            const std::unordered_map<std::string, std::string> &rename_map);
void rewrite_refs_in_type_(ast::TypeNode *t,
                            const std::unordered_map<std::string, std::string> &rename_map);

void rewrite_refs_in_type_(ast::TypeNode *t,
                            const std::unordered_map<std::string, std::string> &rename_map) {
    if (!t) return;
    if (t->kind == ast::NodeKind::NamedTypeNode) {
        auto *nt = static_cast<ast::NamedTypeNode *>(t);
        auto it = rename_map.find(nt->name);
        if (it != rename_map.end()) nt->name = it->second;
        for (auto &ta : nt->type_args) rewrite_refs_in_type_(ta.get(), rename_map);
    } else if (t->kind == ast::NodeKind::PointerTypeNode) {
        auto *pt = static_cast<ast::PointerTypeNode *>(t);
        rewrite_refs_in_type_(pt->pointee.get(), rename_map);
    } else if (t->kind == ast::NodeKind::ArrayTypeNode) {
        auto *at = static_cast<ast::ArrayTypeNode *>(t);
        rewrite_refs_in_type_(at->element_type.get(), rename_map);
    } else if (t->kind == ast::NodeKind::FunctionTypeNode) {
        auto *ft = static_cast<ast::FunctionTypeNode *>(t);
        for (auto &pt : ft->param_types) rewrite_refs_in_type_(pt.get(), rename_map);
        rewrite_refs_in_type_(ft->return_type.get(), rename_map);
    }
}

void rewrite_refs_in_expr_(ast::Expr *e,
                            const std::unordered_map<std::string, std::string> &rename_map) {
    if (!e) return;
    switch (e->kind) {
        case ast::NodeKind::IdentExpr: {
            auto *id = static_cast<ast::IdentExpr *>(e);
            auto it = rename_map.find(id->name);
            if (it != rename_map.end()) id->name = it->second;
            break;
        }
        case ast::NodeKind::CallExpr: {
            auto *c = static_cast<ast::CallExpr *>(e);
            rewrite_refs_in_expr_(c->callee.get(), rename_map);
            for (auto &a : c->args) rewrite_refs_in_expr_(a.get(), rename_map);
            for (auto &ta : c->type_args) rewrite_refs_in_type_(ta.get(), rename_map);
            break;
        }
        case ast::NodeKind::NewExpr: {
            auto *ne = static_cast<ast::NewExpr *>(e);
            auto it = rename_map.find(ne->class_name);
            if (it != rename_map.end()) ne->class_name = it->second;
            for (auto &a : ne->args) rewrite_refs_in_expr_(a.get(), rename_map);
            for (auto &ta : ne->type_args) rewrite_refs_in_type_(ta.get(), rename_map);
            if (ne->array_size) rewrite_refs_in_expr_(ne->array_size.get(), rename_map);
            break;
        }
        case ast::NodeKind::BinaryExpr: {
            auto *b = static_cast<ast::BinaryExpr *>(e);
            rewrite_refs_in_expr_(b->lhs.get(), rename_map);
            rewrite_refs_in_expr_(b->rhs.get(), rename_map);
            break;
        }
        case ast::NodeKind::UnaryExpr: {
            auto *u = static_cast<ast::UnaryExpr *>(e);
            rewrite_refs_in_expr_(u->operand.get(), rename_map);
            break;
        }
        case ast::NodeKind::AssignExpr: {
            auto *a = static_cast<ast::AssignExpr *>(e);
            rewrite_refs_in_expr_(a->target.get(), rename_map);
            rewrite_refs_in_expr_(a->value.get(), rename_map);
            break;
        }
        case ast::NodeKind::IndexExpr: {
            auto *ix = static_cast<ast::IndexExpr *>(e);
            rewrite_refs_in_expr_(ix->base.get(), rename_map);
            rewrite_refs_in_expr_(ix->index.get(), rename_map);
            break;
        }
        case ast::NodeKind::FieldAccessExpr: {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e);
            // Caso especial: si la base es IdentExpr cuyo nombre matchea
            // un enum renamed, reescribir.  Asi `Color.Red` -> `ns__Color.Red`.
            rewrite_refs_in_expr_(fa->base.get(), rename_map);
            break;
        }
        case ast::NodeKind::CastExpr: {
            auto *ce = static_cast<ast::CastExpr *>(e);
            rewrite_refs_in_expr_(ce->operand.get(), rename_map);
            rewrite_refs_in_type_(ce->target_type.get(), rename_map);
            break;
        }
        case ast::NodeKind::TernaryExpr: {
            auto *tn = static_cast<ast::TernaryExpr *>(e);
            rewrite_refs_in_expr_(tn->cond.get(), rename_map);
            rewrite_refs_in_expr_(tn->then_expr.get(), rename_map);
            rewrite_refs_in_expr_(tn->else_expr.get(), rename_map);
            break;
        }
        default: break;
    }
}

void rewrite_refs_in_stmt_(ast::Stmt *s,
                            const std::unordered_map<std::string, std::string> &rename_map) {
    if (!s) return;
    switch (s->kind) {
        case ast::NodeKind::BlockStmt: {
            auto *b = static_cast<ast::BlockStmt *>(s);
            for (auto &c : b->body) rewrite_refs_in_stmt_(c.get(), rename_map);
            break;
        }
        case ast::NodeKind::ExprStmt: {
            auto *es = static_cast<ast::ExprStmt *>(s);
            rewrite_refs_in_expr_(es->expr.get(), rename_map);
            break;
        }
        case ast::NodeKind::VarDeclStmt: {
            auto *vd = static_cast<ast::VarDeclStmt *>(s);
            rewrite_refs_in_type_(vd->type.get(), rename_map);
            if (vd->init) rewrite_refs_in_expr_(vd->init.get(), rename_map);
            break;
        }
        case ast::NodeKind::IfStmt: {
            auto *ifs = static_cast<ast::IfStmt *>(s);
            rewrite_refs_in_expr_(ifs->cond.get(), rename_map);
            rewrite_refs_in_stmt_(ifs->then_branch.get(), rename_map);
            rewrite_refs_in_stmt_(ifs->else_branch.get(), rename_map);
            break;
        }
        case ast::NodeKind::WhileStmt: {
            auto *w = static_cast<ast::WhileStmt *>(s);
            rewrite_refs_in_expr_(w->cond.get(), rename_map);
            rewrite_refs_in_stmt_(w->body.get(), rename_map);
            break;
        }
        case ast::NodeKind::ForStmt: {
            auto *fr = static_cast<ast::ForStmt *>(s);
            rewrite_refs_in_stmt_(fr->init.get(), rename_map);
            rewrite_refs_in_expr_(fr->cond.get(), rename_map);
            rewrite_refs_in_expr_(fr->step.get(), rename_map);
            rewrite_refs_in_stmt_(fr->body.get(), rename_map);
            break;
        }
        case ast::NodeKind::ReturnStmt: {
            auto *r = static_cast<ast::ReturnStmt *>(s);
            rewrite_refs_in_expr_(r->value.get(), rename_map);
            break;
        }
        default: break;
    }
}

/// Mangle interno: aplica el prefix al nombre de cada decl + recorre
/// todos los hijos para aplicar los rewrites.  El @p rename_map se
/// EXTIENDE conforme el walker descubre nombres a renombrar.
void mangle_decls_(std::vector<std::unique_ptr<ast::Node>> &decls,
                    const std::string &ns_path,
                    std::unordered_map<std::string, std::string> &rename_map);

void mangle_function_decl_(ast::FunctionDecl *fd,
                            const std::string &ns_path,
                            std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, fd->name);
    if (newn != fd->name) {
        rename_map.emplace(fd->name, newn);
        fd->name = newn;
    }
    // Params + return: tipos pueden referenciar otros namespace-types.
    rewrite_refs_in_type_(fd->return_type.get(), rename_map);
    for (auto &p : fd->params) {
        // ParamDecl::type es un TypeNode.
        rewrite_refs_in_type_(p->type.get(), rename_map);
    }
    rewrite_refs_in_stmt_(fd->body.get(), rename_map);
}

void mangle_struct_decl_(ast::StructDecl *sd,
                          const std::string &ns_path,
                          std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, sd->name);
    if (newn != sd->name) {
        rename_map.emplace(sd->name, newn);
        sd->name = newn;
    }
    for (auto &f : sd->fields) {
        rewrite_refs_in_type_(f.type.get(), rename_map);
    }
}

void mangle_class_decl_(ast::ClassDecl *cd,
                         const std::string &ns_path,
                         std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, cd->name);
    if (newn != cd->name) {
        rename_map.emplace(cd->name, newn);
        cd->name = newn;
    }
    // Super class y interfaces: si tambien estan en el rename_map, ajustar.
    {
        auto it = rename_map.find(cd->super_name);
        if (it != rename_map.end()) cd->super_name = it->second;
    }
    for (auto &iname : cd->interface_names) {
        auto it = rename_map.find(iname);
        if (it != rename_map.end()) iname = it->second;
    }
    // Fields + metodos.
    for (auto &f : cd->fields) {
        rewrite_refs_in_type_(f.type.get(), rename_map);
    }
    for (auto &m : cd->methods) {
        rewrite_refs_in_type_(m->return_type.get(), rename_map);
        for (auto &p : m->params) rewrite_refs_in_type_(p->type.get(), rename_map);
        rewrite_refs_in_stmt_(m->body.get(), rename_map);
    }
}

void mangle_enum_decl_(ast::EnumDecl *ed,
                        const std::string &ns_path,
                        std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, ed->name);
    if (newn != ed->name) {
        rename_map.emplace(ed->name, newn);
        ed->name = newn;
    }
    // Variantes: pueden tener payload types que apunten a otros namespace-types.
    for (auto &v : ed->variants) {
        for (auto &pt : v.field_types) {
            rewrite_refs_in_type_(pt.get(), rename_map);
        }
    }
}

void mangle_typealias_decl_(ast::TypeAliasDecl *td,
                             const std::string &ns_path,
                             std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, td->name);
    if (newn != td->name) {
        rename_map.emplace(td->name, newn);
        td->name = newn;
    }
    rewrite_refs_in_type_(td->aliased.get(), rename_map);
}

void mangle_global_var_decl_(ast::GlobalVarDecl *gd,
                              const std::string &ns_path,
                              std::unordered_map<std::string, std::string> &rename_map) {
    const std::string newn = mangle_name_(ns_path, gd->name);
    if (newn != gd->name) {
        rename_map.emplace(gd->name, newn);
        gd->name = newn;
    }
    rewrite_refs_in_type_(gd->type.get(), rename_map);
    if (gd->init) rewrite_refs_in_expr_(gd->init.get(), rename_map);
}

void mangle_decls_(std::vector<std::unique_ptr<ast::Node>> &decls,
                    const std::string &ns_path,
                    std::unordered_map<std::string, std::string> &rename_map) {
    // Primera pasada: recolectar los nombres a renombrar (sin tocar bodies
    // todavia).  Asi cuando la segunda pasada reescribe referencias,
    // todos los nombres del namespace ya estan en el rename_map.
    for (auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
            case ast::NodeKind::FunctionDecl: {
                auto *fd = static_cast<ast::FunctionDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, fd->name);
                if (newn != fd->name) rename_map.emplace(fd->name, newn);
                break;
            }
            case ast::NodeKind::StructDecl: {
                auto *sd = static_cast<ast::StructDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, sd->name);
                if (newn != sd->name) rename_map.emplace(sd->name, newn);
                break;
            }
            case ast::NodeKind::ClassDecl: {
                auto *cd = static_cast<ast::ClassDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, cd->name);
                if (newn != cd->name) rename_map.emplace(cd->name, newn);
                break;
            }
            case ast::NodeKind::EnumDecl: {
                auto *ed = static_cast<ast::EnumDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, ed->name);
                if (newn != ed->name) rename_map.emplace(ed->name, newn);
                break;
            }
            case ast::NodeKind::TypeAliasDecl: {
                auto *td = static_cast<ast::TypeAliasDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, td->name);
                if (newn != td->name) rename_map.emplace(td->name, newn);
                break;
            }
            case ast::NodeKind::GlobalVarDecl: {
                auto *gd = static_cast<ast::GlobalVarDecl *>(d.get());
                const std::string newn = mangle_name_(ns_path, gd->name);
                if (newn != gd->name) rename_map.emplace(gd->name, newn);
                break;
            }
            case ast::NodeKind::NamespaceDecl: {
                // Pre-recolectar los nombres del namespace anidado con
                // el prefix combinado.
                auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
                const std::string nested_path = ns_path + "__" + nd->name;
                mangle_decls_(nd->decls, nested_path, rename_map);
                // El namespace decl mismo no se renombra; se procesa al
                // aplanar en collect_and_flatten_.
                break;
            }
            default: break;
        }
    }
    // Segunda pasada: aplicar el rename + reescribir referencias.
    for (auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
            case ast::NodeKind::FunctionDecl:
                mangle_function_decl_(static_cast<ast::FunctionDecl *>(d.get()),
                                       ns_path, rename_map);
                break;
            case ast::NodeKind::StructDecl:
                mangle_struct_decl_(static_cast<ast::StructDecl *>(d.get()),
                                     ns_path, rename_map);
                break;
            case ast::NodeKind::ClassDecl:
                mangle_class_decl_(static_cast<ast::ClassDecl *>(d.get()),
                                    ns_path, rename_map);
                break;
            case ast::NodeKind::EnumDecl:
                mangle_enum_decl_(static_cast<ast::EnumDecl *>(d.get()),
                                   ns_path, rename_map);
                break;
            case ast::NodeKind::TypeAliasDecl:
                mangle_typealias_decl_(static_cast<ast::TypeAliasDecl *>(d.get()),
                                        ns_path, rename_map);
                break;
            case ast::NodeKind::GlobalVarDecl:
                mangle_global_var_decl_(static_cast<ast::GlobalVarDecl *>(d.get()),
                                         ns_path, rename_map);
                break;
            default: break;
        }
    }
}

/// Recolecta los nombres publicos (mangled) que cada namespace contiene
/// + sube los decls al @p out_decls al nivel del Module raiz.  Recursivo
/// para soportar namespaces anidados (las decls del nivel mas interno
/// se suben TODAS al mismo top-level).
void collect_and_flatten_(std::vector<std::unique_ptr<ast::Node>> &in_decls,
                           const std::string &local_ns_name,
                           const std::string &full_path,
                           FlattenedNamespace &out_ns,
                           std::vector<std::unique_ptr<ast::Node>> &out_decls) {
    for (auto &d : in_decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            // Anidado: el nombre del namespace del fichero original sigue
            // siendo el namespace LOCAL (e.g. "controls" dentro de "ui").
            // El path completo del prefix de mangling es ya `ui__controls`.
            auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
            const std::string nested_path = full_path + "__" + nd->name;
            // El namespace anidado se trata como un namespace SEPARADO
            // accesible via `ui.controls.X` (anidamiento de simbolos).
            // En MVP solo soportamos un nivel (ui.X); los simbolos del
            // nivel anidado se agregan al namespace padre con sus nombres
            // mangled completos.
            FlattenedNamespace nested;
            nested.name = local_ns_name + "." + nd->name;  // representacion humana
            collect_and_flatten_(nd->decls, nd->name, nested_path, nested, out_decls);
            // Volcar los simbolos del nested al padre con sus nombres
            // completos (`controls.Button` se accederia como `ui.controls.Button`
            // -- la base sigue siendo el namespace raiz `ui`).  Para MVP
            // soportamos solo notacion `ui_controls_Button` (los hijos
            // se aplanan al padre con el prefix completo en su nombre publico).
            for (auto &sym : nested.symbols) {
                // El nombre publico que se accede como `ui.<X>` donde X
                // es `<nested_name>__<sym_name>`.  E.g. `controls__Button`.
                FlattenedNamespace::Sym child = sym;
                child.public_name = nd->name + "__" + sym.public_name;
                out_ns.symbols.push_back(std::move(child));
            }
            continue;
        }
        // Decls normales: extraer el nombre publico (mangled) + apilar.
        switch (d->kind) {
            case ast::NodeKind::FunctionDecl: {
                auto *fd = static_cast<ast::FunctionDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Function;
                sym.public_name   = (full_path.empty()
                    ? fd->name
                    : fd->name.substr(full_path.size() + 2));
                sym.mangled_label = fd->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            case ast::NodeKind::StructDecl: {
                auto *sd = static_cast<ast::StructDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Type;
                sym.public_name   = (full_path.empty()
                    ? sd->name
                    : sd->name.substr(full_path.size() + 2));
                sym.mangled_label = sd->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            case ast::NodeKind::ClassDecl: {
                auto *cd = static_cast<ast::ClassDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Type;
                sym.public_name   = (full_path.empty()
                    ? cd->name
                    : cd->name.substr(full_path.size() + 2));
                sym.mangled_label = cd->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            case ast::NodeKind::EnumDecl: {
                auto *ed = static_cast<ast::EnumDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Type;
                sym.public_name   = (full_path.empty()
                    ? ed->name
                    : ed->name.substr(full_path.size() + 2));
                sym.mangled_label = ed->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            case ast::NodeKind::TypeAliasDecl: {
                auto *td = static_cast<ast::TypeAliasDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Type;
                sym.public_name   = (full_path.empty()
                    ? td->name
                    : td->name.substr(full_path.size() + 2));
                sym.mangled_label = td->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            case ast::NodeKind::GlobalVarDecl: {
                auto *gd = static_cast<ast::GlobalVarDecl *>(d.get());
                FlattenedNamespace::Sym sym;
                sym.kind          = FlattenedNamespace::Sym::Variable;
                sym.public_name   = (full_path.empty()
                    ? gd->name
                    : gd->name.substr(full_path.size() + 2));
                sym.mangled_label = gd->name;
                out_ns.symbols.push_back(std::move(sym));
                out_decls.push_back(std::move(d));
                break;
            }
            default:
                // Otros decls (ImportDecl, etc.) los pasamos sin modificar.
                out_decls.push_back(std::move(d));
                break;
        }
    }
}

} // namespace

std::vector<FlattenedNamespace> flatten_namespaces(ast::ModuleNode &mod) {
    std::vector<FlattenedNamespace> namespaces;
    // 1. Recorrer top-level: para cada NamespaceDecl, hacer mangling
    //    interno + extraer decls.
    std::vector<std::unique_ptr<ast::Node>> new_decls;
    new_decls.reserve(mod.decls.size() * 2);

    for (auto &d : mod.decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            auto *nd = static_cast<ast::NamespaceDecl *>(d.get());
            // Mangle todos los decls internos con el prefijo del namespace.
            std::unordered_map<std::string, std::string> rename_map;
            mangle_decls_(nd->decls, nd->name, rename_map);
            // Recolectar simbolos publicos + subir decls al top-level.
            FlattenedNamespace ns;
            ns.name = nd->name;
            collect_and_flatten_(nd->decls, nd->name, nd->name, ns, new_decls);
            namespaces.push_back(std::move(ns));
            // El NamespaceDecl wrapper se descarta (no se anyade a new_decls).
            // Sus decls internos ya estan en new_decls con nombres mangled.
        } else {
            new_decls.push_back(std::move(d));
        }
    }

    mod.decls = std::move(new_decls);
    return namespaces;
}

} // namespace vex
