/*
 * VestaVM - include/install/platform.h
 *
 * Interfaz abstracta del backend de plataforma. Cada plataforma implementa
 * estas operaciones con sus APIs nativas (winreg.h en Windows, .desktop +
 * xdg-mime en Linux). El orquestador (install.cpp) llama solo a esta API.
 */

#ifndef VESTA_INSTALL_PLATFORM_H
#define VESTA_INSTALL_PLATFORM_H

#include <filesystem>
#include <string>

#include "install/install_options.h"
#include "install/manifest.h"

namespace install {

    /**
     * @brief Backend de plataforma. Implementacion seleccionada en compilacion.
     */
    class Platform {
    public:
        virtual ~Platform() = default;

        // ---- privilegios ----
        /// True si el proceso corre con permisos de admin/root.
        virtual bool is_elevated() const = 0;

        /// Mensaje informativo para el wizard ("Run as Administrator", "sudo", etc.).
        virtual std::string elevation_hint() const = 0;

        // ---- rutas por defecto ----
        virtual std::filesystem::path default_prefix(Scope scope) const = 0;
        virtual std::filesystem::path default_bin_dir(const std::filesystem::path& prefix) const = 0;
        virtual std::filesystem::path default_share_dir(const std::filesystem::path& prefix) const = 0;

        // ---- copia de ficheros ----
        /// Copia source_dir/* a opts.prefix segun los toggles, registrando en mf.
        virtual bool copy_files(const InstallOptions& opts, Manifest& mf) = 0;

        // ---- asociaciones de extension ----
        /// Asocia opts.associations con el binario instalado.
        virtual bool register_associations(const InstallOptions& opts, Manifest& mf) = 0;

        // ---- PATH ----
        virtual bool add_to_path(const InstallOptions& opts, Manifest& mf) = 0;

        // ---- shortcuts / start menu / .desktop ----
        virtual bool create_shortcuts(const InstallOptions& opts, Manifest& mf) = 0;

        // ---- registro de desinstalador ----
        virtual bool register_uninstaller(const InstallOptions& opts, Manifest& mf) = 0;

        // ---- desinstalacion: deshacer cada seccion del manifest ----
        virtual bool unregister_associations(const Manifest& mf) = 0;
        virtual bool remove_from_path(const Manifest& mf) = 0;
        virtual bool remove_shortcuts(const Manifest& mf) = 0;
        virtual bool unregister_uninstaller(const Manifest& mf) = 0;
        virtual bool remove_files(const Manifest& mf, bool keep_user_data) = 0;

        // ---- post-instalacion ----
        /// Notifica al sistema (Explorer/desktop env) de los cambios.
        virtual void notify_system() = 0;
    };

    /**
     * @brief Factory: devuelve el backend para la plataforma actual.
     */
    Platform& current_platform();

} // namespace install

#endif // VESTA_INSTALL_PLATFORM_H
