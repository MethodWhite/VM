/**
 * @file paths.h
 * @brief Resolucion de rutas estandar del package manager Vesta.
 *
 * Tres ambitos de instalacion:
 *   - Proyecto (`./vex_modules/`): default; vive junto al `vex.toml`.
 *   - Usuario (`$VEX_HOME` o `~/.vesta`): cache + keys + paquetes globales
 *     del usuario actual.
 *   - Sistema: directorio donde se instalo la VM (`ProgramFiles/Vesta` Win,
 *     `/usr/local/share/vesta` POSIX); compartido entre todos los usuarios.
 *
 * Resolucion de VEX_HOME (en orden):
 *   1. Variable de entorno @c VEX_HOME explicita.
 *   2. Per-usuario: @c %APPDATA%/Vesta (Win) o @c $HOME/.vesta (POSIX).
 *   3. Sistema (read-only desde el punto de vista del PM, salvo `pkg install --system`).
 *
 * Estructura tipica de @c $VEX_HOME:
 *   $VEX_HOME/
 *     cache/             # paquetes descargados (read-only tras verify)
 *     packages/          # paquetes "instalados globalmente"
 *     keys/              # claves publicas confiables (kpub1:*.pem)
 *     keys/private/      # claves privadas del usuario (chmod 600)
 *     registry/          # mirrors de indices (futuro)
 */
#ifndef VESTAVM_PKG_PATHS_H
#define VESTAVM_PKG_PATHS_H

#include <string>
#include <vector>

namespace pkg::paths {

    /**
     * @brief Ambito de instalacion de un paquete.
     */
    enum class Scope {
        Project,   ///< @c ./vex_modules/ (default; junto al vex.toml)
        User,      ///< @c $VEX_HOME (per-usuario)
        System     ///< Directorio de la VM (sysadmin only)
    };

    /**
     * @brief Devuelve el path canonico de @c $VEX_HOME (per-usuario).
     *
     * Si la env var @c VEX_HOME esta definida, la devuelve normalizada.
     * Si no, devuelve @c $APPDATA/Vesta (Win) o @c $HOME/.vesta (POSIX).
     */
    std::string vex_home();

    /**
     * @brief Devuelve el path del directorio system-wide donde se instalo la VM.
     *
     * Calculado a partir del path del ejecutable (@c GetModuleFileName /
     * @c readlink("/proc/self/exe")).  Si no se puede determinar, devuelve
     * cadena vacia.
     */
    std::string system_install_dir();

    /**
     * @brief Devuelve el directorio @c vex_modules del proyecto actual.
     *
     * Busca @c vex.toml o @c vex.json subiendo desde @p start_dir.  Si no
     * encuentra manifest, devuelve cadena vacia.
     */
    std::string project_modules_dir(const std::string &start_dir);

    /**
     * @brief Devuelve el directorio raiz del proyecto (donde vive el manifest).
     */
    std::string project_root(const std::string &start_dir);

    /**
     * @brief Devuelve el directorio @c packages de un scope dado.
     */
    std::string packages_dir(Scope scope, const std::string &project_root = "");

    /**
     * @brief Devuelve el directorio @c cache de un scope dado.
     */
    std::string cache_dir(Scope scope, const std::string &project_root = "");

    /**
     * @brief Devuelve el directorio @c keys publicas del usuario.
     */
    std::string keys_dir();

    /**
     * @brief Devuelve el directorio @c keys/private del usuario (chmod 600).
     */
    std::string private_keys_dir();

    /**
     * @brief Asegura que el directorio @p path existe, creandolo recursivamente.
     */
    bool ensure_dir(const std::string &path);

    /**
     * @brief Normaliza un path (resuelve `..`, `/`, etc.).
     */
    std::string normalize(const std::string &path);

    /**
     * @brief Une dos componentes de path con el separador correcto.
     */
    std::string join(const std::string &a, const std::string &b);

    /**
     * @brief Path al manifest del paquete instalado.
     *
     * @c <scope>/packages/<name>@<version>/vex.toml
     */
    std::string installed_manifest(Scope scope,
                                    const std::string &name,
                                    const std::string &version,
                                    const std::string &project_root = "");

    /**
     * @brief True si @p path existe (file o directorio).
     */
    bool exists(const std::string &path);

    /**
     * @brief True si @p path es un directorio.
     */
    bool is_directory(const std::string &path);

    /**
     * @brief True si @p path es un archivo regular.
     */
    bool is_file(const std::string &path);

} // namespace pkg::paths

#endif // VESTAVM_PKG_PATHS_H
