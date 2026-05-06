/*
 * VestaVM - include/install/manifest.h
 *
 * Manifest de instalacion: registra cada fichero, clave de registro, symlink
 * y entrada de PATH creada para que el desinstalador deshaga exactamente lo
 * que hizo el instalador. Formato JSON plano para que sea inspeccionable.
 */

#ifndef VESTA_INSTALL_MANIFEST_H
#define VESTA_INSTALL_MANIFEST_H

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace install {

    struct ManifestFile {
        std::filesystem::path path;
        uint64_t              size   = 0;
        std::string           sha256;       ///< Vacio si no se calculo
    };

    struct ManifestRegistry {
        std::string hive;        ///< "HKLM" o "HKCU"
        std::string key;         ///< Subclave (sin hive)
        std::string value_name;  ///< "" para (Default)
        std::string data;
    };

    struct ManifestSymlink {
        std::filesystem::path link;
        std::filesystem::path target;
    };

    struct ManifestPathEntry {
        std::string entry;       ///< Lo que se anadio al PATH
        std::string scope;       ///< "user" / "system"
    };

    struct ManifestShortcut {
        std::string           kind;   ///< "start_menu" / "desktop" / "applications"
        std::filesystem::path path;
    };

    struct ManifestDesktopFile {
        std::filesystem::path path;   ///< ej: ~/.local/share/applications/vesta-vsh.desktop
    };

    struct ManifestMimeFile {
        std::filesystem::path path;   ///< ej: ~/.local/share/mime/packages/vesta.xml
    };

    /**
     * @brief Manifest completo. Un fichero por instalacion en
     *        <prefix>/install_manifest.json
     */
    struct Manifest {
        std::string version          = "1.0";
        std::string vesta_version;
        std::string install_date;            ///< ISO-8601 UTC
        std::string platform;                ///< "windows" / "linux"
        std::string scope;                   ///< "user" / "system"
        std::filesystem::path prefix;

        std::vector<ManifestFile>        files;
        std::vector<ManifestRegistry>    registry;
        std::vector<ManifestSymlink>     symlinks;
        std::vector<ManifestPathEntry>   path_entries;
        std::vector<ManifestShortcut>    shortcuts;
        std::vector<ManifestDesktopFile> desktop_files;
        std::vector<ManifestMimeFile>    mime_files;

        /// Donde se guarda este manifest
        std::filesystem::path manifest_path() const;

        /// Persistencia
        bool save() const;
        bool save_to(const std::filesystem::path& path) const;
        bool load_from(const std::filesystem::path& path);
    };

} // namespace install

#endif // VESTA_INSTALL_MANIFEST_H
