/**
 * @file auditor.h
 * @brief Auditoria del grafo de dependencias instalado.
 *
 * Detecta cambios sospechosos en autor, capabilities o sha256 vs lockfile
 * previo.  Tipos de hallazgos:
 *   - AUTHOR_CHANGED: la fingerprint del autor cambio entre versiones.
 *   - SHA_CHANGED: el sha256 del contenido cambio.  Catastrofico si el
 *     lockfile dice version X y la dep instalada en X tiene otro hash.
 *   - CAPS_EXPANDED: el paquete declara MAS capabilities que la version
 *     anterior (e.g. antes solo FS_READ, ahora FS_WRITE+NET).
 *   - UNSAFE: el paquete marca explicitamente que es @c unsafe.
 *   - UNSIGNED: el paquete no esta firmado por nadie pinneado.
 */
#ifndef VESTAVM_PKG_AUDITOR_H
#define VESTAVM_PKG_AUDITOR_H

#include "pkg/lockfile.h"
#include "pkg/signature.h"

#include <string>
#include <vector>

namespace pkg::audit {

    enum class Severity { Info, Warning, Critical };

    struct Finding {
        Severity    severity;
        std::string code;          ///< AUTHOR_CHANGED, SHA_CHANGED, etc.
        std::string package_name;
        std::string description;
        std::string suggestion;
    };

    struct AuditReport {
        std::vector<Finding> findings;
        size_t       critical_count = 0;
        size_t       warning_count  = 0;
        size_t       info_count     = 0;
    };

    /**
     * @brief Audita un lockfile actual vs uno previo.
     *
     * @param current   lockfile recien resuelto
     * @param previous  lockfile cacheado (puede estar vacio)
     * @param pins      trust pins del usuario
     */
    AuditReport audit_lockfile(const Lockfile &current,
                                const Lockfile &previous,
                                const std::vector<signing::TrustPin> &pins);

    /**
     * @brief Pretty-print del reporte usando @c pkg::ui colores.
     */
    void print_report(const AuditReport &report);

} // namespace pkg::audit

#endif // VESTAVM_PKG_AUDITOR_H
