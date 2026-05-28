/**
 * @file module_interop.h
 * @brief Interop entre TypeChecker y formato @c .vexi (Phase M.2.d).
 *
 * Funciones libres que conectan el estado del @c TypeChecker con un
 * @c VexiModule.  Viven fuera del TypeChecker para no forzar
 * @c vexi_format.h en todos sus consumidores.
 */

#ifndef VEX_MODULE_INTEROP_H
#define VEX_MODULE_INTEROP_H

#include <cstdint>
#include <vector>

#include "vex/type_checker.h"

namespace vex {

struct VexiModule;        // fwd decl, definido en vexi_format.h

/**
 * @brief Extrae los simbolos publicos del @c TypeChecker a un
 * @c VexiModule listo para serializar.
 *
 * En el MVP M2, "publico" = todos los typedefs, structs, classes,
 * enums y funciones top-level del modulo.  La regla de @c public /
 * @c private explicito llega en M6.
 *
 * @param tc           TypeChecker con el modulo ya verificado.
 * @param source_hash  Hash FNV-1a 64 del source crudo (para
 *                     invalidacion incremental del cache).
 * @param out          @c VexiModule destino.  Se sobrescribe.
 */
void export_typechecker_to_vexi(const TypeChecker &tc,
                                  uint64_t source_hash,
                                  VexiModule &out,
                                  const std::string &strip_prefix = "");

/**
 * @brief Inyecta simbolos importados de un @c VexiModule en las tablas
 * del TypeChecker actual.
 *
 * En el MVP M2 solo se inyectan los simbolos LISTADOS en
 * @c only_symbols.  Los imports plain (`import "x";`) o con alias
 * (`import "x" as a;`) requieren namespace support, pendiente en M2.x.
 *
 * @param tc           TypeChecker destino.
 * @param mod          Modulo @c VexiModule decodificado.
 * @param only_symbols Lista de simbolos a importar (con rename opcional).
 *                     Si vacio: no se inyecta nada.
 */
void import_vexi_into_typechecker(
        TypeChecker &tc,
        const VexiModule &mod,
        const std::vector<TypeChecker::VexiOnlyEntry> &only_symbols);

/**
 * @brief Variante que devuelve la lista de simbolos solicitados pero
 * NO encontrados (o privados) en el `.vexi`.  Util para que el caller
 * (compile_vex_project) emita diagnosticos cross-module precisos
 * (Phase M6.a L.3).
 */
std::vector<std::string> import_vexi_into_typechecker_with_missing(
        TypeChecker &tc,
        const VexiModule &mod,
        const std::vector<TypeChecker::VexiOnlyEntry> &only_symbols);

/**
 * @brief Phase M.7: registra un namespace para un @c "import \"lib\";"
 * o @c "import \"lib\" as alias;" plain (sin @c only).
 *
 * @param tc          TypeChecker del consumidor.
 * @param local_name  Nombre visible (alias o module_name si sin alias).
 * @param module_name Nombre original del modulo (para mensajes de error).
 * @param mod         Modulo @c VexiModule decodificado del dep.
 *
 * Tras esta llamada, @c local_name esta registrado como un
 * Symbol::Namespace en el scope global del consumidor.  El usuario puede
 * acceder a sus simbolos con la sintaxis @c local_name.simbolo.
 */
void register_namespace_for_import(
        TypeChecker &tc,
        const std::string &local_name,
        const std::string &module_name,
        const VexiModule &mod);

} // namespace vex

#endif // VEX_MODULE_INTEROP_H
