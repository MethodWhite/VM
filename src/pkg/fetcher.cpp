/**
 * @file fetcher.cpp
 * @brief Implementacion del fetcher de paquetes con verificacion sha256.
 */
#include "pkg/fetcher.h"
#include "pkg/sha256.h"
#include "pkg/paths.h"
#include "pkg/ui.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace pkg::fetcher {

    namespace {
        /**
         * @brief Ejecuta un comando del sistema y retorna stdout + exit code.
         */
        struct CmdResult {
            int         exit_code = -1;
            std::string output;
        };

        CmdResult run_cmd(const std::string &cmd) {
            CmdResult r;
#ifdef _WIN32
            FILE *p = _popen(cmd.c_str(), "r");
#else
            FILE *p = popen(cmd.c_str(), "r");
#endif
            if (!p) return r;
            char buf[4096];
            while (fgets(buf, sizeof(buf), p)) {
                r.output.append(buf);
            }
#ifdef _WIN32
            r.exit_code = _pclose(p);
#else
            r.exit_code = pclose(p);
#endif
            return r;
        }

        /**
         * @brief Quote-friendly de un argumento para shell.  Conservador:
         *        wrap en comillas dobles + escape de @c " interno.
         */
        std::string sh_quote(const std::string &arg) {
            std::string out;
            out.reserve(arg.size() + 8);
            out += '"';
            for (char c : arg) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
            return out;
        }

        FetchResult fail(const std::string &msg) {
            FetchResult r;
            r.ok = false;
            r.error_msg = msg;
            return r;
        }
    } // namespace

    bool git_available() {
        // Una llamada simple para chequear que git esta en el PATH.
#ifdef _WIN32
        return std::system("git --version >NUL 2>&1") == 0;
#else
        return std::system("git --version >/dev/null 2>&1") == 0;
#endif
    }

    SourceSpec resolve_github_import(const std::string &import_path) {
        SourceSpec s;
        s.kind = SourceSpec::Github;
        // Formato: github.com/owner/repo[@ref]
        std::string p = import_path;
        std::string ref;
        auto at = p.find('@');
        if (at != std::string::npos) {
            ref = p.substr(at + 1);
            p = p.substr(0, at);
        }
        // Validacion minima.
        if (p.rfind("github.com/", 0) != 0) {
            s.url.clear();
            return s;
        }
        // Convertir a clone URL.
        std::string slug = p.substr(std::string("github.com/").size());
        s.url = "https://github.com/" + slug + ".git";
        if (!ref.empty()) {
            // Heuristica: si comienza con v y digito es tag, hexadecimal largo es rev,
            // si no asumimos branch.
            if (ref.size() >= 7) {
                bool all_hex = true;
                for (char c : ref) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        all_hex = false;
                        break;
                    }
                }
                if (all_hex) {
                    s.rev = ref;
                    return s;
                }
            }
            if (!ref.empty() && (ref[0] == 'v' || std::isdigit(static_cast<unsigned char>(ref[0])))) {
                s.tag = ref;
            } else {
                s.branch = ref;
            }
        } else {
            s.branch = "main";   // default Github moderno
        }
        return s;
    }

    bool remove_directory(const std::string &path) {
        if (path.empty()) return false;
        std::error_code ec;
        fs::remove_all(path, ec);
        return !ec;
    }

    namespace {
        FetchResult fetch_git(const SourceSpec &spec, const std::string &dest_dir) {
            FetchResult r;
            if (!git_available()) {
                return fail("git no esta disponible en PATH; instala git para fetch de paquetes");
            }
            // Limpia el destino si existe.
            std::error_code ec;
            fs::remove_all(dest_dir, ec);

            // Asegurar parent.
            fs::create_directories(fs::path(dest_dir).parent_path(), ec);

            // Build clone command: shallow por defecto.  Si hay rev, fetch + checkout.
            std::string cmd = "git clone --depth 1 ";
            if (!spec.tag.empty()) {
                cmd += "--branch " + sh_quote(spec.tag) + " ";
            } else if (!spec.branch.empty()) {
                cmd += "--branch " + sh_quote(spec.branch) + " ";
            }
            cmd += sh_quote(spec.url) + " " + sh_quote(dest_dir);
#ifdef _WIN32
            cmd += " >NUL 2>&1";
#else
            cmd += " >/dev/null 2>&1";
#endif
            ui::Spinner sp("clonando " + spec.url);
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                sp.fail("git clone fallo (rc=" + std::to_string(rc) + ")");
                remove_directory(dest_dir);
                return fail("git clone fallo para " + spec.url);
            }
            sp.done("OK");

            // Si hay rev exacto, hacer fetch full + checkout.
            if (!spec.rev.empty()) {
                std::string fetch_cmd = "cd " + sh_quote(dest_dir) +
                    " && git fetch --unshallow >NUL 2>&1 && git checkout " +
                    sh_quote(spec.rev);
#ifdef _WIN32
                fetch_cmd += " >NUL 2>&1";
#else
                fetch_cmd += " >/dev/null 2>&1";
#endif
                if (std::system(fetch_cmd.c_str()) != 0) {
                    remove_directory(dest_dir);
                    return fail("git checkout " + spec.rev + " fallo");
                }
                r.resolved_rev = spec.rev;
            } else {
                // Resolver el HEAD actual.
                CmdResult cr = run_cmd("cd " + sh_quote(dest_dir) +
                                        " && git rev-parse HEAD");
                if (cr.exit_code == 0 && !cr.output.empty()) {
                    std::string rev = cr.output;
                    while (!rev.empty() && (rev.back() == '\n' || rev.back() == '\r')) {
                        rev.pop_back();
                    }
                    r.resolved_rev = rev;
                }
            }

            // Borrar .git para reducir size + evitar tracking en el cache.
            fs::remove_all(fs::path(dest_dir) / ".git", ec);

            // Hash final del tree.
            r.sha256 = hash::sha256_tree(dest_dir);
            if (r.sha256.empty()) {
                remove_directory(dest_dir);
                return fail("no se pudo computar sha256 del paquete");
            }

            // Si el spec declara expected_sha, verificar.
            if (!spec.expected_sha.empty()) {
                if (!hash::hash_equal_ct(spec.expected_sha, r.sha256)) {
                    remove_directory(dest_dir);
                    return fail("sha256 mismatch:\n  esperado: " + spec.expected_sha +
                                  "\n  recibido: " + r.sha256 +
                                  "\n  posible tampering detectado, abortando");
                }
            }

            r.installed_at = dest_dir;
            r.ok = true;
            return r;
        }

        FetchResult fetch_path(const SourceSpec &spec, const std::string &dest_dir) {
            FetchResult r;
            if (!paths::is_directory(spec.url)) {
                return fail("path local no es un directorio: " + spec.url);
            }
            // Para deps locales no copiamos: dejamos un symlink/junction si es posible.
            // Si falla el symlink, hacemos copy_recursive como fallback.
            std::error_code ec;
            fs::remove_all(dest_dir, ec);
            fs::create_directories(fs::path(dest_dir).parent_path(), ec);

            fs::create_directory_symlink(spec.url, dest_dir, ec);
            if (ec) {
                // Fallback: copia recursiva.
                fs::copy(spec.url, dest_dir,
                          fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    return fail("no se pudo copiar el path local: " + ec.message());
                }
            }

            r.installed_at = dest_dir;
            r.resolved_rev = "local";
            r.sha256 = hash::sha256_tree(dest_dir);
            r.ok = true;
            // Para path local NO hace falta verificacion sha (es del desarrollador).
            return r;
        }
    } // namespace

    FetchResult fetch(const SourceSpec &spec, const std::string &dest_dir) {
        switch (spec.kind) {
            case SourceSpec::Git:
            case SourceSpec::Github:
                return fetch_git(spec, dest_dir);
            case SourceSpec::Path:
                return fetch_path(spec, dest_dir);
            case SourceSpec::Url:
            case SourceSpec::Zip:
                // El fetcher http+tarball requiere libcurl o equivalente.  Por ahora
                // devolvemos error claro y dejamos el hook para implementar despues.
                return fail("fetch URL/zip no implementado en MVP; usa git+https://... o path local");
        }
        return fail("source kind desconocido");
    }

} // namespace pkg::fetcher
