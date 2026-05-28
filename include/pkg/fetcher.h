/**
 * @file fetcher.h
 * @brief Descarga de dependencias desde multiples fuentes con verificacion.
 *
 * Soporta:
 *   - git+url@rev / @branch / @tag (via @c git clone delegado al sistema)
 *   - https+url + sha256 obligatorio (tarball)
 *   - path local (symlink o copy)
 *   - github.com/usuario/repo estilo Go (auto-resolve a git+https://...)
 *
 * Verificacion estricta:
 *   - sha256 del directorio extraido vs el declarado en lockfile/manifest.
 *   - Si no coincide, aborta y limpia el directorio temporal.
 *   - Las descargas pasan primero por @c $VEX_HOME/cache para reutilizar.
 *
 * Sin postinstall scripts.  El paquete fetched es PURO codigo Vex.
 */
#ifndef VESTAVM_PKG_FETCHER_H
#define VESTAVM_PKG_FETCHER_H

#include <string>
#include <vector>

namespace pkg::fetcher {

    /**
     * @brief Resultado de una operacion de fetch.
     */
    struct FetchResult {
        bool        ok = false;
        std::string installed_at;    ///< Path final donde quedo el paquete
        std::string resolved_rev;    ///< Commit/ref exacto (para git)
        std::string sha256;          ///< Hash del contenido descargado
        std::string error_msg;
    };

    /**
     * @brief Especificacion de fuente para un fetch.
     */
    struct SourceSpec {
        enum Kind {
            Git,        ///< git+https://... + rev/branch/tag
            Url,        ///< https+ tarball + sha256 obligatorio
            Path,       ///< path local (./...)
            Github,     ///< github.com/owner/repo (Go-style, resuelve a Git)
            Zip         ///< tarball local o url+sha256
        };
        Kind        kind = Path;
        std::string url;          ///< git url, https url, o path local
        std::string rev;          ///< commit hash exacto (preferible)
        std::string branch;       ///< rama (mutable; warning)
        std::string tag;          ///< tag (recomendado)
        std::string expected_sha; ///< sha256 esperado (obligatorio para Url/Zip)
    };

    /**
     * @brief Verifica que @c git este disponible en el PATH del sistema.
     */
    bool git_available();

    /**
     * @brief Descarga un paquete a @p dest_dir segun @p spec.
     *
     * Algoritmo:
     *   1. Si dest_dir ya existe y sha256 coincide -> reutilizar.
     *   2. Si no, fetch al cache temporal, verificar sha256, mover a dest_dir.
     *   3. Devolver hash final + path + rev resuelto.
     *
     * NO ejecuta ningun script post-install del paquete (anti-malware).
     */
    FetchResult fetch(const SourceSpec &spec, const std::string &dest_dir);

    /**
     * @brief Auto-resuelve `github.com/owner/repo[@ref]` estilo Go a `SourceSpec`.
     *
     * Convierte `import "github.com/foo/bar"` a:
     *   { kind=Git, url="https://github.com/foo/bar.git", branch="main" }
     */
    SourceSpec resolve_github_import(const std::string &import_path);

    /**
     * @brief Elimina recursivamente un directorio.  Util tras un fetch fallido.
     */
    bool remove_directory(const std::string &path);

} // namespace pkg::fetcher

#endif // VESTAVM_PKG_FETCHER_H
