/**
 * @file lockfile.cpp
 * @brief Implementacion del parser/serializer de @c vex.lock.
 */
#include "pkg/lockfile.h"
#include "pkg/toml_lite.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace pkg {

    namespace {

        /**
         * @brief Extrae un string opcional de una @c TomlTable.
         */
        std::string read_str(const toml::TomlTable &t, const std::string &key,
                              const std::string &def = "") {
            auto it = t.find(key);
            if (it == t.end()) return def;
            if (!it->second.is_string()) return def;
            return it->second.as_string();
        }

        bool read_bool(const toml::TomlTable &t, const std::string &key,
                        bool def = false) {
            auto it = t.find(key);
            if (it == t.end()) return def;
            if (!it->second.is_bool()) return def;
            return it->second.as_bool();
        }

        std::vector<std::string> read_str_array(const toml::TomlTable &t,
                                                  const std::string &key) {
            std::vector<std::string> out;
            auto it = t.find(key);
            if (it == t.end() || !it->second.is_array()) return out;
            for (const auto &v : it->second.as_array()) {
                if (v.is_string()) out.push_back(v.as_string());
            }
            return out;
        }

        LockEntry parse_entry(const toml::TomlTable &t) {
            LockEntry e;
            e.name           = read_str(t, "name");
            e.version        = read_str(t, "version");
            e.source         = read_str(t, "source");
            e.resolved_rev   = read_str(t, "resolved_rev");
            e.sha256         = read_str(t, "sha256");
            e.author_fp      = read_str(t, "author_fp");
            e.signature_alg  = read_str(t, "signature_alg", "ed25519");
            e.unsafe         = read_bool(t, "unsafe", false);
            e.declared_caps  = read_str_array(t, "declared_caps");
            e.deps           = read_str_array(t, "deps");
            e.install_at     = read_str(t, "install_at", "project");
            return e;
        }

    } // namespace

    LockParseResult parse_lockfile_buffer(const std::string &buffer) {
        LockParseResult res;
        auto pr = toml::parse(buffer);
        if (!pr.ok) {
            res.error_msg = "lockfile invalido: " + pr.error_msg +
                            " (linea " + std::to_string(pr.error_line) + ")";
            return res;
        }
        // [meta]
        auto meta_it = pr.root.find("meta");
        if (meta_it != pr.root.end() && meta_it->second.is_table()) {
            const auto &mt = meta_it->second.as_table();
            res.lock.meta.version      = read_str(mt, "version", "1");
            res.lock.meta.generated_at = read_str(mt, "generated_at");
            res.lock.meta.resolver     = read_str(mt, "resolver", "mvr-1");
        }
        // [[package]]
        auto pk_it = pr.root.find("package");
        if (pk_it != pr.root.end() && pk_it->second.is_array()) {
            for (const auto &v : pk_it->second.as_array()) {
                if (!v.is_table()) continue;
                res.lock.packages.push_back(parse_entry(v.as_table()));
            }
        }
        res.ok = true;
        return res;
    }

    LockParseResult parse_lockfile_file(const std::string &path) {
        LockParseResult res;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            res.error_msg = "no se pudo abrir: " + path;
            return res;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return parse_lockfile_buffer(ss.str());
    }

    std::string serialize_lockfile(const Lockfile &lock) {
        std::ostringstream out;
        out << "# vex.lock -- generado automaticamente, no editar a mano\n";
        out << "# Builds reproducibles bit-perfect: sha256 + author_fp + caps.\n\n";

        // [meta] -- escape_string ya wrapea con "..."
        out << "[meta]\n";
        out << "version       = " << toml::escape_string(lock.meta.version) << "\n";
        out << "generated_at  = " << toml::escape_string(lock.meta.generated_at) << "\n";
        out << "resolver      = " << toml::escape_string(lock.meta.resolver) << "\n\n";

        // Ordenar packages por name+version para output estable.
        std::vector<LockEntry> sorted = lock.packages;
        std::sort(sorted.begin(), sorted.end(), [](const LockEntry &a, const LockEntry &b) {
            if (a.name != b.name) return a.name < b.name;
            return a.version < b.version;
        });

        for (const auto &p : sorted) {
            out << "[[package]]\n";
            out << "name           = " << toml::escape_string(p.name) << "\n";
            out << "version        = " << toml::escape_string(p.version) << "\n";
            out << "source         = " << toml::escape_string(p.source) << "\n";
            if (!p.resolved_rev.empty())
                out << "resolved_rev   = " << toml::escape_string(p.resolved_rev) << "\n";
            if (!p.sha256.empty())
                out << "sha256         = " << toml::escape_string(p.sha256) << "\n";
            if (!p.author_fp.empty())
                out << "author_fp      = " << toml::escape_string(p.author_fp) << "\n";
            out << "signature_alg  = " << toml::escape_string(p.signature_alg) << "\n";
            out << "unsafe         = " << (p.unsafe ? "true" : "false") << "\n";

            // Arrays: en una linea si pocos elementos.
            auto emit_arr = [&](const char *key, const std::vector<std::string> &arr) {
                out << key << " = [";
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i > 0) out << ", ";
                    out << toml::escape_string(arr[i]);
                }
                out << "]\n";
            };
            emit_arr("declared_caps ", p.declared_caps);
            emit_arr("deps          ", p.deps);

            out << "install_at     = " << toml::escape_string(p.install_at) << "\n\n";
        }
        return out.str();
    }

    std::string current_utc_iso8601() {
        // Implementacion portable sin <format> (C++17).
        std::time_t t = std::time(nullptr);
        std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    const LockEntry *find_package(const Lockfile &lock, const std::string &name) {
        for (const auto &p : lock.packages) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }

} // namespace pkg
