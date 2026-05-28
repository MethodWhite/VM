/**
 * @file namespace_flatten.h
 * @brief Pre-pass que aplana @c NamespaceDecl inline (Phase M.7.c).
 *
 * Tras parsear el `.vex`, el AST contiene @c NamespaceDecl nodes
 * envolviendo grupos de declaraciones.  Este pre-pass:
 *   1. Manglea los nombres internos con prefix @c <ns>__.
 *   2. Reescribe referencias internas a los nombres mangled.
 *   3. Sube los decls al top-level del Module.
 *   4. Devuelve descriptores de cada namespace encontrado para que el
 *      caller los registre como Symbol::Namespace en el TypeChecker.
 */

#ifndef VEX_NAMESPACE_FLATTEN_H
#define VEX_NAMESPACE_FLATTEN_H

#include <string>
#include <vector>

namespace vex {

namespace ast { struct ModuleNode; }

/**
 * @brief Descriptor de un namespace inline aplanado.
 *
 * Los simbolos llevan @c public_name (nombre visible desde fuera del
 * namespace, e.g. @c "Button") y @c mangled_label (nombre interno
 * efectivo en el bytecode, e.g. @c "ui__Button").
 */
struct FlattenedNamespace {
    std::string name;       ///< nombre del namespace (e.g. "ui").

    struct Sym {
        enum Kind {
            Function = 0,
            Type     = 1,    ///< struct, class, enum, typedef
            Variable = 2,
        };
        Kind        kind;
        std::string public_name;     ///< visible: "Button"
        std::string mangled_label;   ///< interno: "ui__Button"
    };

    std::vector<Sym> symbols;
};

/**
 * @brief Aplana todos los @c NamespaceDecl inline del modulo.
 *
 * Modifica @p mod en-place: tras la llamada, @c mod.decls NO contiene
 * @c NamespaceDecl; los contenidos se han movido al top-level con sus
 * nombres mangled.
 *
 * @return Lista de namespaces encontrados (ordenados por aparicion).
 *         Vacio si no hay namespaces inline.
 */
std::vector<FlattenedNamespace> flatten_namespaces(ast::ModuleNode &mod);

} // namespace vex

#endif // VEX_NAMESPACE_FLATTEN_H
