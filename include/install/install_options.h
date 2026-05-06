/*
 * VestaVM - Maquina Virtual Distribuida
 * include/install/install_options.h
 *
 * Estructura central de opciones del instalador. Todos los campos tienen
 * valores por defecto sensatos; el wizard interactivo o los flags de linea
 * de comandos los modifican antes de ejecutar la instalacion.
 */

#ifndef VESTA_INSTALL_OPTIONS_H
#define VESTA_INSTALL_OPTIONS_H

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace install {

    /**
     * @brief Opciones por extension de fichero a asociar.
     */
    struct AssocOpt {
        bool        enabled       = true;
        bool        create_icon   = true;
        std::string description;     ///< Texto que aparece en el explorador
        std::string verb_open      = "Open";   ///< Verbo principal
        std::string command_template;           ///< Argumentos: "--script \"%1\"", "--run \"%1\""
    };

    /**
     * @brief Tipo de instalacion segun privilegios.
     */
    enum class Scope {
        PerUser,    ///< Sin admin: AppData/Local o ~/.local
        SystemWide  ///< Con admin: Program Files o /usr/local
    };

    /**
     * @brief Conjunto completo de opciones del instalador.
     *
     * El wizard arranca con los defaults derivados del entorno (detecta admin,
     * calcula prefix segun scope). El usuario puede modificar cualquier campo
     * antes de confirmar. El struct se persiste tal cual en el manifest para
     * que el desinstalador sepa exactamente que se hizo.
     */
    struct InstallOptions {
        // ------------------------------------------------------------------
        // Destino
        // ------------------------------------------------------------------
        Scope                  scope  = Scope::PerUser;
        std::filesystem::path  prefix;       ///< Calculado segun scope si esta vacio
        std::filesystem::path  bin_dir;      ///< prefix/bin (Linux) o prefix (Windows)
        std::filesystem::path  share_dir;    ///< prefix/share (Linux) o prefix (Windows)

        // ------------------------------------------------------------------
        // Origen de los ficheros (auto-detectado)
        // ------------------------------------------------------------------
        std::filesystem::path  source_dir;   ///< Donde esta el binario actual ejecutandose

        // ------------------------------------------------------------------
        // Que copiar
        // ------------------------------------------------------------------
        bool copy_binary      = true;
        bool copy_examples    = true;
        bool copy_docs        = true;
        bool copy_icons       = true;
        bool copy_stdlib      = true;
        bool copy_vscode_ext  = false;       ///< Crea symlink/copia de ext_code/vsh

        // ------------------------------------------------------------------
        // Asociaciones de fichero
        // ------------------------------------------------------------------
        std::map<std::string, AssocOpt> associations = {
            { ".velb", { true,  true,  "Vesta Bytecode",       "Open",   "--run \"%1\""    } },
            { ".vsh",  { true,  true,  "Vesta Shell Script",   "Open",   "--script \"%1\"" } },
            { ".vel",  { false, false, "Vesta Source",         "Edit",   ""                } },
        };

        // ------------------------------------------------------------------
        // Integracion del sistema
        // ------------------------------------------------------------------
        bool add_to_path             = true;
        bool create_start_menu       = true;   ///< Windows
        bool create_desktop_shortcut = false;
        bool register_uninstaller    = true;   ///< Windows: Add/Remove Programs
        bool create_man_pages        = true;   ///< Linux

        // ------------------------------------------------------------------
        // Modo de operacion
        // ------------------------------------------------------------------
        bool silent      = false;   ///< Sin preguntas, todo defaults
        bool dry_run     = false;   ///< Mostrar pasos sin ejecutarlos
        bool force       = false;   ///< Sobreescribir si ya existe
        bool verbose     = false;
        bool no_color    = false;

        /**
         * @brief Calcula prefix, bin_dir y share_dir segun el scope si estan vacios.
         *
         * Tambien rellena source_dir con el directorio del ejecutable actual.
         */
        void resolve_paths();

        /**
         * @brief Imprime un resumen legible de las opciones (para el wizard).
         */
        void print_summary() const;
    };

} // namespace install

#endif // VESTA_INSTALL_OPTIONS_H
