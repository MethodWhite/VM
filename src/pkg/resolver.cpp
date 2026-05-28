/**
 * @file resolver.cpp
 * @brief Resolver MVR + helpers semver.
 */
#include "pkg/resolver.h"
#include "pkg/fetcher.h"
#include "pkg/sha256.h"
#include "pkg/paths.h"
#include "pkg/ui.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <queue>

namespace pkg::resolver {

    namespace {
        /**
         * @brief Split simple por @c '.'.  No valida; el caller debe.
         */
        std::vector<int> parse_semver(const std::string &v, bool *pre_release = nullptr) {
            std::vector<int> out;
            std::string cur;
            std::string ver = v;
            // Quita prefijo "v" si existe.
            if (!ver.empty() && ver[0] == 'v') ver.erase(0, 1);
            // Cualquier sufijo "-rc", "-beta", etc. lo separamos.
            auto dash = ver.find('-');
            if (dash != std::string::npos) {
                if (pre_release) *pre_release = true;
                ver = ver.substr(0, dash);
            }
            for (char c : ver) {
                if (c == '.') {
                    if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
                    cur.clear();
                } else if (std::isdigit(static_cast<unsigned char>(c))) {
                    cur.push_back(c);
                } else {
                    break;
                }
            }
            if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
            // Padding hasta 3 componentes.
            while (out.size() < 3) out.push_back(0);
            return out;
        }
    } // namespace

    int semver_compare(const std::string &a, const std::string &b) {
        auto va = parse_semver(a);
        auto vb = parse_semver(b);
        for (size_t i = 0; i < va.size() && i < vb.size(); ++i) {
            if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
        }
        if (va.size() != vb.size()) return va.size() < vb.size() ? -1 : 1;
        return 0;
    }

    bool semver_satisfies(const std::string &version, const std::string &req) {
        if (req.empty() || req == "*" || req == "latest") return true;
        // Caret: "^1.2" -> >=1.2.0, <2.0.0
        if (req[0] == '^') {
            std::string base = req.substr(1);
            auto vb = parse_semver(base);
            auto vv = parse_semver(version);
            if (vv[0] != vb[0]) return false;
            return semver_compare(version, base) >= 0;
        }
        // Tilde: "~1.2.3" -> >=1.2.3, <1.3.0
        if (req[0] == '~') {
            std::string base = req.substr(1);
            auto vb = parse_semver(base);
            auto vv = parse_semver(version);
            if (vv[0] != vb[0] || vv[1] != vb[1]) return false;
            return semver_compare(version, base) >= 0;
        }
        // >= y >
        if (req.size() > 2 && req[0] == '>' && req[1] == '=') {
            return semver_compare(version, req.substr(2)) >= 0;
        }
        if (!req.empty() && req[0] == '>') {
            return semver_compare(version, req.substr(1)) > 0;
        }
        // Exact: "1.2.3" or "1.2"
        // Permitimos x.y == x.y.0
        auto va = parse_semver(version);
        auto vb = parse_semver(req);
        for (size_t i = 0; i < 3; ++i) {
            if (va[i] != vb[i]) return false;
        }
        return true;
    }

    namespace {
        /**
         * @brief Convierte una @c DependencySpec del manifest a @c SourceSpec.
         */
        fetcher::SourceSpec spec_from_dep(const DependencySpec &d) {
            fetcher::SourceSpec s;
            if (!d.path.empty()) {
                s.kind = fetcher::SourceSpec::Path;
                s.url  = d.path;
            } else if (!d.git_url.empty()) {
                s.kind = fetcher::SourceSpec::Git;
                s.url  = d.git_url;
                s.rev  = d.git_rev;
                s.expected_sha = d.sha256;
            } else if (d.name.rfind("github.com/", 0) == 0) {
                s = fetcher::resolve_github_import(d.name);
                s.expected_sha = d.sha256;
            } else {
                s.kind = fetcher::SourceSpec::Url;
                s.url  = d.name;
                s.expected_sha = d.sha256;
            }
            return s;
        }
    } // namespace

    ResolveResult resolve(const Manifest &root,
                           const std::string &work_dir,
                           const std::vector<signing::TrustPin> &trust_pins,
                           bool allow_unsigned) {
        ResolveResult result;
        if (!root.valid()) {
            result.error_msg = "manifest raiz invalido (sin name o version)";
            return result;
        }

        // Tabla deduplicada por nombre.  MVR: nos quedamos con la version MAS
        // ALTA de las requeridas + el resto del spec del primer matcher.
        std::unordered_map<std::string, ResolvedDep> picked;
        std::queue<DependencySpec> queue;

        for (const auto &d : root.dependencies) queue.push(d);

        size_t guard = 0;
        while (!queue.empty()) {
            if (++guard > 4096) {
                result.error_msg = "resolucion excedio 4096 iteraciones; ciclo o tree explosivo";
                return result;
            }
            DependencySpec d = queue.front();
            queue.pop();

            auto it = picked.find(d.name);
            if (it != picked.end()) {
                // Ya picked: aplicar MVR (subir version si es mayor).
                if (!d.version.empty() &&
                    semver_compare(d.version, it->second.version) > 0) {
                    it->second.version = d.version;
                }
                continue;
            }

            ResolvedDep rd;
            rd.name        = d.name;
            rd.version     = d.version;
            rd.source_url  = !d.path.empty() ? d.path :
                              !d.git_url.empty() ? d.git_url : d.name;
            rd.unsafe      = false;
            rd.author_fp   = d.trust_key;

            // Para path local, leer su propio manifest y encolar sus deps.
            if (!d.path.empty()) {
                std::string mani_path = paths::join(d.path, "vex.toml");
                if (!paths::exists(mani_path)) {
                    mani_path = paths::join(d.path, "vex.json");
                }
                if (paths::exists(mani_path)) {
                    auto pr = parse_manifest_file(mani_path);
                    if (pr.ok) {
                        rd.version = pr.manifest.version;
                        rd.declared_caps = pr.manifest.capabilities.declared;
                        for (const auto &td : pr.manifest.dependencies) {
                            rd.transitive_deps.push_back(td.name);
                            queue.push(td);
                        }
                    } else {
                        result.warnings.push_back("path " + d.path +
                            ": manifest invalido, " + pr.error_msg);
                    }
                }
                rd.resolved_rev = "local";
            } else if (!d.git_url.empty() || d.name.rfind("github.com/", 0) == 0) {
                // Fetch a un subdir temporal del work_dir, leer manifest, recursar.
                std::string sub = paths::join(work_dir, "_resolve");
                paths::ensure_dir(sub);
                std::string dest = paths::join(sub, d.name);
                std::replace(dest.begin(), dest.end(), '/', '_');

                fetcher::SourceSpec src = spec_from_dep(d);
                auto fr = fetcher::fetch(src, dest);
                if (!fr.ok) {
                    result.warnings.push_back("no se pudo fetchear " + d.name +
                        ": " + fr.error_msg);
                    continue;
                }
                rd.resolved_rev = fr.resolved_rev;
                rd.source_url   = src.url;

                std::string mani_path = paths::join(dest, "vex.toml");
                if (!paths::exists(mani_path)) {
                    mani_path = paths::join(dest, "vex.json");
                }
                if (paths::exists(mani_path)) {
                    auto pr = parse_manifest_file(mani_path);
                    if (pr.ok) {
                        if (rd.version.empty()) rd.version = pr.manifest.version;
                        rd.declared_caps = pr.manifest.capabilities.declared;
                        for (const auto &td : pr.manifest.dependencies) {
                            rd.transitive_deps.push_back(td.name);
                            queue.push(td);
                        }
                    } else {
                        result.warnings.push_back("dep " + d.name +
                            ": manifest invalido, " + pr.error_msg);
                    }
                } else {
                    result.warnings.push_back("dep " + d.name +
                        ": sin vex.toml / vex.json en el repo");
                }
            } else {
                result.warnings.push_back("dep " + d.name +
                    ": fuente desconocida (sin path ni git), saltada");
                continue;
            }

            // Validacion anti-malware: si la dep declara que es unsafe Y el
            // usuario no ha pinneado, warning.
            if (!rd.author_fp.empty()) {
                if (!signing::is_trusted(rd.author_fp, trust_pins)) {
                    if (allow_unsigned) {
                        result.warnings.push_back("dep " + d.name +
                            ": autor " + rd.author_fp + " no pinneado (TOFU)");
                    } else {
                        result.warnings.push_back("dep " + d.name +
                            ": autor " + rd.author_fp + " no pinneado; usa --allow-unsigned o pin manual");
                    }
                }
            } else if (!allow_unsigned) {
                result.warnings.push_back("dep " + d.name +
                    ": sin firma (sin trust_key); usa --allow-unsigned para permitir");
            }

            picked[d.name] = rd;
        }

        for (auto &kv : picked) result.deps.push_back(kv.second);
        std::sort(result.deps.begin(), result.deps.end(),
                  [](const ResolvedDep &a, const ResolvedDep &b) {
                      return a.name < b.name;
                  });

        result.ok = true;
        return result;
    }

    Lockfile build_lockfile(const ResolveResult &res) {
        Lockfile lock;
        lock.meta.generated_at = current_utc_iso8601();
        for (const auto &d : res.deps) {
            LockEntry e;
            e.name           = d.name;
            e.version        = d.version;
            e.source         = d.source_url;
            e.resolved_rev   = d.resolved_rev;
            e.author_fp      = d.author_fp;
            e.signature_alg  = "ed25519";
            e.unsafe         = d.unsafe;
            e.declared_caps  = d.declared_caps;
            e.deps           = d.transitive_deps;
            e.install_at     = "project";
            lock.packages.push_back(e);
        }
        return lock;
    }

} // namespace pkg::resolver
