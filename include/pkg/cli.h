/**
 * @file cli.h
 * @brief Despachador del CLI @c vm pkg <subcomando> [args].
 *
 * Subcomandos soportados:
 *   init [--json]                       Crear vex.toml/vex.json en cwd
 *   add <name> [--git URL --rev R | --path P]
 *                                       Anyadir dep al manifest + lockfile
 *   remove <name>                       Quitar dep
 *   install [--allow-unsigned]          Fetch + lockfile + install todo
 *   update                              Actualizar lockfile segun semver
 *   list                                Mostrar arbol de deps
 *   verify                              Verificar sha256/firma de cada dep
 *   audit                               Auditoria comparativa
 *   convert toml|json                   Convertir formato del manifest
 *   trust list                          Mostrar trust pins
 *   trust add <fp> [--name N]           Anyadir pin
 *   trust revoke <fp>                   Revocar pin
 *   keygen [--out PATH]                 Generar par de claves Ed25519
 *   sign <package_dir>                  Firmar paquete con clave privada
 *   verify-sig <package_dir> <fp>       Verificar firma del paquete
 *   run <script>                        Ejecutar script declarado en manifest
 */
#ifndef VESTAVM_PKG_CLI_H
#define VESTAVM_PKG_CLI_H

#include <string>
#include <vector>

namespace pkg::cli {

    /**
     * @brief Punto de entrada principal.
     *
     * @param argc/argv  igual que un main; argv[0] = "pkg".
     * @return codigo de salida (0 ok, !=0 error).
     */
    int run(int argc, char **argv);

    /**
     * @brief Helper para ejecutar via vector<string>.
     */
    int run_args(const std::vector<std::string> &args);

} // namespace pkg::cli

#endif // VESTAVM_PKG_CLI_H
