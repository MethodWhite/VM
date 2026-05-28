/**
 * @file auditor.cpp
 * @brief Implementacion de la auditoria comparativa del lockfile.
 */
#include "pkg/auditor.h"
#include "pkg/ui.h"

#include <iostream>
#include <unordered_set>

namespace pkg::audit {

    namespace {
        void bump_counts(AuditReport &r, const Finding &f) {
            switch (f.severity) {
                case Severity::Critical: r.critical_count++; break;
                case Severity::Warning:  r.warning_count++;  break;
                case Severity::Info:     r.info_count++;     break;
            }
            r.findings.push_back(f);
        }
    } // namespace

    AuditReport audit_lockfile(const Lockfile &current,
                                const Lockfile &previous,
                                const std::vector<signing::TrustPin> &pins) {
        AuditReport report;

        // Indexar previo por nombre.
        std::unordered_map<std::string, const LockEntry *> prev_by_name;
        for (const auto &p : previous.packages) {
            prev_by_name[p.name] = &p;
        }

        for (const auto &c : current.packages) {
            // Check 1: unsafe marker explicito.
            if (c.unsafe) {
                Finding f;
                f.severity     = Severity::Warning;
                f.code         = "UNSAFE";
                f.package_name = c.name;
                f.description  = "el paquete se declara @c unsafe (acceso bajo nivel)";
                f.suggestion   = "revisa el codigo manualmente antes de install";
                bump_counts(report, f);
            }

            // Check 2: unsigned (sin author_fp o no pinneado).
            if (c.author_fp.empty()) {
                Finding f;
                f.severity     = Severity::Warning;
                f.code         = "UNSIGNED";
                f.package_name = c.name;
                f.description  = "el paquete no tiene firma criptografica";
                f.suggestion   = "considera pinear un autor confiable o usa solo deps verificadas";
                bump_counts(report, f);
            } else if (!signing::is_trusted(c.author_fp, pins)) {
                Finding f;
                f.severity     = Severity::Warning;
                f.code         = "UNTRUSTED_AUTHOR";
                f.package_name = c.name;
                f.description  = "autor " + c.author_fp + " no esta en tu trust pin list";
                f.suggestion   = "ejecuta `vm pkg trust add " + c.author_fp + "` si confias en este autor";
                bump_counts(report, f);
            }

            // Check 3: caps expandidas vs version anterior.
            auto it = prev_by_name.find(c.name);
            if (it != prev_by_name.end()) {
                const LockEntry *p = it->second;

                // Author change: la fingerprint cambio.
                if (!p->author_fp.empty() && !c.author_fp.empty() &&
                    p->author_fp != c.author_fp) {
                    Finding f;
                    f.severity     = Severity::Critical;
                    f.code         = "AUTHOR_CHANGED";
                    f.package_name = c.name;
                    f.description  = "el autor cambio entre " + p->version +
                                      " (" + p->author_fp + ") y " + c.version +
                                      " (" + c.author_fp + ")";
                    f.suggestion   = "posible takeover; verifica manualmente con el publisher original";
                    bump_counts(report, f);
                }

                // SHA change con misma version: catastrofico.
                if (p->version == c.version && !p->sha256.empty() &&
                    !c.sha256.empty() && p->sha256 != c.sha256) {
                    Finding f;
                    f.severity     = Severity::Critical;
                    f.code         = "SHA_CHANGED";
                    f.package_name = c.name;
                    f.description  = "version identica (" + c.version +
                                      ") con sha256 distinto, tampering posible";
                    f.suggestion   = "abortar build y reportar al publisher";
                    bump_counts(report, f);
                }

                // Caps expandidas: counter previo.
                std::unordered_set<std::string> prev_caps(
                    p->declared_caps.begin(), p->declared_caps.end());
                std::vector<std::string> new_caps;
                for (const auto &cap : c.declared_caps) {
                    if (prev_caps.find(cap) == prev_caps.end()) {
                        new_caps.push_back(cap);
                    }
                }
                if (!new_caps.empty()) {
                    Finding f;
                    f.severity     = Severity::Warning;
                    f.code         = "CAPS_EXPANDED";
                    f.package_name = c.name;
                    std::string desc = "el paquete pide capabilities nuevas:";
                    for (const auto &nc : new_caps) {
                        desc += "\n      - " + nc;
                    }
                    f.description = desc;
                    f.suggestion  = "revisa que el cambio sea legitimo";
                    bump_counts(report, f);
                }
            }
        }

        return report;
    }

    namespace {
        const char *severity_color(Severity s) {
            switch (s) {
                case Severity::Critical: return ui::red();
                case Severity::Warning:  return ui::yellow();
                case Severity::Info:     return ui::cyan();
            }
            return ui::reset();
        }

        const char *severity_label(Severity s) {
            switch (s) {
                case Severity::Critical: return "[CRITICAL]";
                case Severity::Warning:  return "[WARNING] ";
                case Severity::Info:     return "[INFO]    ";
            }
            return "[?]";
        }
    } // namespace

    void print_report(const AuditReport &report) {
        ui::header("Auditoria de dependencias");

        if (report.findings.empty()) {
            ui::ok("sin hallazgos: todas las dependencias verificadas");
            return;
        }

        for (const auto &f : report.findings) {
            std::cout << severity_color(f.severity)
                       << severity_label(f.severity) << ui::reset()
                       << " " << ui::magenta() << f.package_name << ui::reset()
                       << "  " << ui::bold() << f.code << ui::reset() << "\n";
            std::cout << ui::gray() << "  " << f.description << ui::reset() << "\n";
            if (!f.suggestion.empty()) {
                std::cout << ui::cyan() << "  hint: " << ui::reset()
                           << f.suggestion << "\n";
            }
            std::cout << "\n";
        }

        std::cout << ui::bold() << "Resumen:" << ui::reset() << " ";
        if (report.critical_count > 0) {
            std::cout << ui::red() << report.critical_count
                       << " critical" << ui::reset() << "  ";
        }
        if (report.warning_count > 0) {
            std::cout << ui::yellow() << report.warning_count
                       << " warnings" << ui::reset() << "  ";
        }
        if (report.info_count > 0) {
            std::cout << ui::cyan() << report.info_count
                       << " info" << ui::reset();
        }
        std::cout << "\n";
    }

} // namespace pkg::audit
