/*
 * VestaVM - src/install/install_options.cpp
 */

#include "install/install_options.h"
#include "install/platform.h"

#include <iostream>
#include <iomanip>
#include <cstdlib>

namespace install {

    void InstallOptions::resolve_paths() {
        Platform& plat = current_platform();

        if (prefix.empty()) {
            prefix = plat.default_prefix(scope);
        }
        if (bin_dir.empty()) {
            bin_dir = plat.default_bin_dir(prefix);
        }
        if (share_dir.empty()) {
            share_dir = plat.default_share_dir(prefix);
        }
        if (source_dir.empty()) {
            // El backend sabe como localizar el binario actual; aqui dejamos
            // un fallback razonable: el directorio de trabajo.
            std::error_code ec;
            source_dir = std::filesystem::current_path(ec);
        }
    }

    void InstallOptions::print_summary() const {
        auto yn = [](bool b) { return b ? "Si" : "No"; };
        auto scope_str = [&]() {
            return scope == Scope::PerUser ? "Per-user" : "System-wide";
        };

        std::cout << "\n";
        std::cout << " ============================================================\n";
        std::cout << "   Resumen de instalacion\n";
        std::cout << " ============================================================\n";
        std::cout << "  [1] Tipo de instalacion ........ " << scope_str() << "\n";
        std::cout << "  [2] Destino .................... " << prefix.string() << "\n";
        std::cout << "  [3] Asociar .velb .............. " << yn(associations.at(".velb").enabled) << "\n";
        std::cout << "  [4] Asociar .vsh ............... " << yn(associations.at(".vsh").enabled) << "\n";
        std::cout << "  [5] Asociar .vel ............... " << yn(associations.at(".vel").enabled) << "\n";
        std::cout << "  [6] Anadir al PATH ............. " << yn(add_to_path) << "\n";
        std::cout << "  [7] Acceso directo escritorio .. " << yn(create_desktop_shortcut) << "\n";
        std::cout << "  [8] Menu inicio (Win) .......... " << yn(create_start_menu) << "\n";
        std::cout << "  [9] Registrar desinstalador .... " << yn(register_uninstaller) << "\n";
        std::cout << "  [a] Copiar ejemplos ............ " << yn(copy_examples) << "\n";
        std::cout << "  [b] Copiar documentacion ....... " << yn(copy_docs) << "\n";
        std::cout << "  [c] Copiar extension VS Code ... " << yn(copy_vscode_ext) << "\n";
        std::cout << " ------------------------------------------------------------\n";
        std::cout << "   [I]nstalar  [Q]uit  [<num/letra>] cambiar opcion\n";
        std::cout << " ============================================================\n";
    }

} // namespace install
