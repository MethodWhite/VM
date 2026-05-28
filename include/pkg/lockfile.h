/**
 * @file lockfile.h
 * @brief Formato @c vex.lock para builds reproducibles bit-perfect.
 *
 * El lockfile congela las decisiones de resolucion: por cada dep transitiva
 * registra @c name + @c version + fuente exacta (rev/sha256) + autor +
 * firma + caps declarados.  El comando @c vm pkg install consume @c vex.lock
 * cuando existe (sin re-resolver semver); si no existe, lo genera tras la
 * primera resolucion exitosa.
 *
 * Formato: TOML estricto (mismo subset que el manifest).  No editable a mano.
 *
 * Esquema:
 *   [meta]
 *   version       = "1"            # version del schema del lockfile
 *   generated_at  = "2026-05-26T...Z"
 *   resolver      = "mvr-1"        # minimum version resolution v1
 *
 *   [[package]]
 *   name           = "@vesta/buffer"
 *   version        = "1.2.5"
 *   source         = "git+https://github.com/vesta/buffer.git"
 *   resolved_rev   = "abc123..."    # commit hash exacto
 *   sha256         = "<64 hex>"     # checksum del tarball/tree
 *   author_fp      = "kpub1:..."    # huella del autor (ed25519)
 *   signature_alg  = "ed25519"
 *   unsafe         = false          # declarado en manifest del paquete
 *   declared_caps  = ["FS_READ=/tmp"]
 *   deps           = ["@vesta/io@2.1.0", ...]
 *   install_at     = "project"      # project|user|system
 */
#ifndef VESTAVM_PKG_LOCKFILE_H
#define VESTAVM_PKG_LOCKFILE_H

#include <string>
#include <vector>
#include <unordered_map>

namespace pkg {

    /**
     * @brief Entrada de un paquete resuelto en el lockfile.
     */
    struct LockEntry {
        std::string name;
        std::string version;
        std::string source;           ///< git+url, https+url, path
        std::string resolved_rev;     ///< commit hash o version-rev exacto
        std::string sha256;           ///< checksum del contenido descargado
        std::string author_fp;        ///< huella publica autor (kpub1:...)
        std::string signature_alg;    ///< "ed25519" hoy
        bool        unsafe = false;
        std::vector<std::string> declared_caps;
        std::vector<std::string> deps;
        std::string install_at;       ///< "project" | "user" | "system"
    };

    /**
     * @brief Metadata global del lockfile.
     */
    struct LockMeta {
        std::string version = "1";
        std::string generated_at;     ///< ISO-8601 UTC
        std::string resolver = "mvr-1";
    };

    /**
     * @brief Estructura completa del lockfile.
     */
    struct Lockfile {
        LockMeta meta;
        std::vector<LockEntry> packages;
    };

    /**
     * @brief Resultado de parse_lockfile.
     */
    struct LockParseResult {
        bool        ok = false;
        Lockfile    lock;
        std::string error_msg;
    };

    /**
     * @brief Parsea @c vex.lock desde un buffer TOML.
     */
    LockParseResult parse_lockfile_buffer(const std::string &buffer);

    /**
     * @brief Parsea @c vex.lock desde archivo.
     */
    LockParseResult parse_lockfile_file(const std::string &path);

    /**
     * @brief Serializa un @c Lockfile a TOML canonico.
     *
     * Orden estable: meta primero, luego packages ordenados por name+version.
     */
    std::string serialize_lockfile(const Lockfile &lock);

    /**
     * @brief Devuelve el timestamp ISO-8601 UTC actual ("2026-05-26T12:34:56Z").
     */
    std::string current_utc_iso8601();

    /**
     * @brief Busca una entrada por nombre exacto en el lockfile.
     *
     * @return Puntero a la entrada o @c nullptr si no existe.
     */
    const LockEntry *find_package(const Lockfile &lock, const std::string &name);

} // namespace pkg

#endif // VESTAVM_PKG_LOCKFILE_H
