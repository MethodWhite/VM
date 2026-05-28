/**
 * @file resolver.h
 * @brief Resolver de dependencias estilo Go modules (MVR).
 *
 * Minimum Version Resolution: para cada dep transitiva, elige la version
 * MINIMA que satisface todos los constraints semver del grafo.  Es
 * deterministico (no requiere SAT solver) y reproducible bit-perfect cuando
 * se combina con el lockfile.
 *
 * Algoritmo:
 *   1. Empezar con el manifest raiz.
 *   2. Por cada dep declarada, registrar (name, min_version).
 *   3. Fetch del manifest de cada dep (a memoria, no install todavia).
 *   4. Por cada dep transitiva, aplicar @c max(min_version_actual, version_del_constraint).
 *   5. Repetir hasta fixpoint.
 *   6. Output: lista plana de paquetes con versions resueltas + sus fuentes.
 */
#ifndef VESTAVM_PKG_RESOLVER_H
#define VESTAVM_PKG_RESOLVER_H

#include "pkg/manifest.h"
#include "pkg/lockfile.h"
#include "pkg/signature.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace pkg::resolver {

    struct ResolvedDep {
        std::string         name;
        std::string         version;
        std::string         source_url;     // git+https://..., path/..., etc.
        std::string         resolved_rev;
        std::vector<std::string> declared_caps;
        std::vector<std::string> transitive_deps;
        bool                unsafe = false;
        std::string         author_fp;
    };

    struct ResolveResult {
        bool                ok = false;
        std::vector<ResolvedDep> deps;
        std::string         error_msg;
        std::vector<std::string> warnings;
    };

    /**
     * @brief Resuelve el grafo de dependencias a partir de un manifest raiz.
     *
     * @param root_manifest    manifest del proyecto consumidor
     * @param work_dir         carpeta temporal para fetches intermedios
     * @param trust_pins       lista de autores confiables (puede estar vacia)
     * @param allow_unsigned   permitir paquetes sin firma (TOFU)
     */
    ResolveResult resolve(const Manifest &root_manifest,
                           const std::string &work_dir,
                           const std::vector<signing::TrustPin> &trust_pins = {},
                           bool allow_unsigned = false);

    /**
     * @brief Compara dos versiones semver.  Devuelve <0/0/>0.
     */
    int  semver_compare(const std::string &a, const std::string &b);

    /**
     * @brief True si @p version satisface el constraint @p req.
     *
     * Subset soportado:
     *   - exact:   "1.2.3"
     *   - caret:   "^1.2"     -> >=1.2.0, <2.0.0
     *   - tilde:   "~1.2.3"   -> >=1.2.3, <1.3.0
     *   - range:   ">=1.0, <2.0"
     *   - latest:  "*"
     */
    bool semver_satisfies(const std::string &version, const std::string &req);

    /**
     * @brief Convierte el resolve result en un @c Lockfile listo para escribir.
     */
    Lockfile build_lockfile(const ResolveResult &res);

} // namespace pkg::resolver

#endif // VESTAVM_PKG_RESOLVER_H
