/*
 * VestaVM - include/install/install.h
 *
 * API publica del instalador. Dos puntos de entrada:
 *   - run_install_cli(argc, argv): para `vm --install ...` desde main.cpp
 *   - run_install_repl(args):      para `install` desde el REPL
 *
 * Ambos parsean opciones, ejecutan el wizard si procede, y delegan en
 * install::execute() o install::uninstall().
 */

#ifndef VESTA_INSTALL_H
#define VESTA_INSTALL_H

#include <string>
#include <vector>

#include "install/install_options.h"
#include "install/manifest.h"

namespace install {

    /**
     * @brief Ejecuta una instalacion completa segun las opciones.
     *
     * Si opts.silent == false y stdin es tty, abre el wizard interactivo.
     * Genera y persiste el manifest.
     *
     * @return Codigo de salida estilo Unix (0 = ok).
     */
    int execute(InstallOptions opts);

    /**
     * @brief Ejecuta una desinstalacion completa.
     *
     * Localiza el manifest (en orden):
     *   1. Argumento --manifest=<path>
     *   2. <prefix>/install_manifest.json
     *   3. Auto-deteccion segun el binario actual
     */
    int uninstall(const std::filesystem::path& manifest_path,
                  bool keep_user_data = true,
                  bool silent = false);

    /**
     * @brief Repara una instalacion (re-asocia, re-PATH) sin recopiar.
     */
    int repair(const std::filesystem::path& manifest_path, bool silent = false);

    /**
     * @brief Punto de entrada para el flag de linea de comandos --install.
     *
     * Acepta: --install, --uninstall, --repair, --prefix, --per-user,
     *         --system-wide, --silent, --dry-run, --no-path,
     *         --assoc=ext1,ext2, --no-assoc=ext1,ext2,
     *         --no-start-menu, --no-uninstaller-entry,
     *         --manifest=<path>
     */
    int run_install_cli(int argc, char** argv);

    /**
     * @brief Punto de entrada para el subcomando del REPL: `install [args]`.
     */
    void run_install_repl(const std::string& args);

    /**
     * @brief Punto de entrada para el subcomando del REPL: `uninstall [args]`.
     */
    void run_uninstall_repl(const std::string& args);

} // namespace install

#endif // VESTA_INSTALL_H
