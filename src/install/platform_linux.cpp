/*
 * VestaVM - src/install/platform_linux.cpp
 *
 * Backend Linux del instalador.
 *
 *   Per-user (sin sudo):
 *     ~/.local/lib/vesta/         -> binario, recursos
 *     ~/.local/bin/vesta          -> symlink al binario
 *     ~/.local/share/applications -> .desktop por extension
 *     ~/.local/share/mime/packages-> MIME types
 *     ~/.local/share/icons        -> iconos
 *
 *   System-wide (con sudo):
 *     /usr/local/lib/vesta
 *     /usr/local/bin/vesta
 *     /usr/share/applications, /usr/share/mime/packages, /usr/share/icons
 *
 * El registro de la asociacion lo hacen los gestores XDG; nosotros
 * generamos los ficheros y llamamos a `update-mime-database` y
 * `update-desktop-database` al final.
 */

#if !defined(_WIN32)

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>

#include "install/platform.h"
#include "install/manifest.h"
#include "install/install_options.h"

namespace install {

    // ====================================================================
    // Helpers
    // ====================================================================

    static std::filesystem::path home_dir() {
        if (const char* h = std::getenv("HOME")) return std::filesystem::path(h);
        struct passwd* pw = getpwuid(getuid());
        return pw ? std::filesystem::path(pw->pw_dir) : std::filesystem::path("/tmp");
    }

    /// Localiza el binario actual a traves de /proc/self/exe.
    static std::filesystem::path module_path() {
        std::error_code ec;
        auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) return std::filesystem::current_path();
        return p;
    }

    /// Run shell command, devuelve exit code. Output a stderr para visibilidad.
    static int run_cmd(const std::string& cmd) {
        return std::system(cmd.c_str());
    }

    static bool copy_dir_recursive(const std::filesystem::path& src,
                                    const std::filesystem::path& dst,
                                    Manifest& mf, bool overwrite)
    {
        std::error_code ec;
        std::filesystem::create_directories(dst, ec);
        for (auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
            if (ec) return false;
            auto rel = std::filesystem::relative(entry.path(), src, ec);
            auto target = dst / rel;
            if (entry.is_directory()) {
                std::filesystem::create_directories(target, ec);
            } else {
                std::filesystem::create_directories(target.parent_path(), ec);
                auto opt = overwrite ? std::filesystem::copy_options::overwrite_existing
                                     : std::filesystem::copy_options::skip_existing;
                std::filesystem::copy_file(entry.path(), target, opt, ec);
                if (!ec) {
                    ManifestFile e;
                    e.path = target;
                    e.size = std::filesystem::file_size(target, ec);
                    mf.files.push_back(e);
                }
            }
        }
        return true;
    }

    // ====================================================================
    // PlatformLinux
    // ====================================================================

    class PlatformLinux : public Platform {
    public:
        bool is_elevated() const override { return geteuid() == 0; }
        std::string elevation_hint() const override {
            return "Re-ejecuta este comando con sudo: sudo vesta install --system-wide";
        }

        std::filesystem::path default_prefix(Scope scope) const override {
            if (scope == Scope::SystemWide) return "/usr/local/lib/vesta";
            return home_dir() / ".local" / "lib" / "vesta";
        }
        std::filesystem::path default_bin_dir(const std::filesystem::path& /*prefix*/) const override {
            // bin separado: distinto del prefix
            if (geteuid() == 0) return "/usr/local/bin";
            return home_dir() / ".local" / "bin";
        }
        std::filesystem::path default_share_dir(const std::filesystem::path& /*prefix*/) const override {
            if (geteuid() == 0) return "/usr/local/share/vesta";
            return home_dir() / ".local" / "share" / "vesta";
        }

        // ---- copia de ficheros + symlink al bin ----
        bool copy_files(const InstallOptions& opts, Manifest& mf) override {
            std::error_code ec;
            std::filesystem::create_directories(opts.prefix, ec);
            std::filesystem::create_directories(opts.bin_dir, ec);
            std::filesystem::create_directories(opts.share_dir, ec);

            // 1) binario al prefix (lib/vesta) y symlink en bin_dir
            if (opts.copy_binary) {
                auto src = module_path();
                auto dst = opts.prefix / "vesta";
                std::filesystem::copy_file(src, dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) { std::cerr << "[copy] " << ec.message() << "\n"; return false; }
                std::filesystem::permissions(dst,
                    std::filesystem::perms::owner_all |
                    std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
                    std::filesystem::perm_options::replace, ec);
                ManifestFile e; e.path = dst;
                e.size = std::filesystem::file_size(dst, ec);
                mf.files.push_back(e);

                auto link = opts.bin_dir / "vesta";
                std::filesystem::remove(link, ec);
                std::filesystem::create_symlink(dst, link, ec);
                if (!ec) mf.symlinks.push_back({ link, dst });
            }

            auto src_root = module_path().parent_path();

            auto try_copy = [&](const char* name, bool enabled) {
                if (!enabled) return;
                auto s = src_root / name;
                if (!std::filesystem::exists(s)) return;
                copy_dir_recursive(s, opts.share_dir / name, mf, opts.force);
            };

            try_copy("doc",                opts.copy_docs);
            try_copy("examples_codes_vsh", opts.copy_examples);
            try_copy("examples_codes_vm",  opts.copy_examples);
            try_copy("stdlib",             opts.copy_stdlib);
            try_copy("icons",              opts.copy_icons);

            // icono individual
            if (opts.copy_icons) {
                auto icon_src = src_root / "icono.png";
                if (std::filesystem::exists(icon_src)) {
                    auto icon_root = (geteuid() == 0)
                        ? std::filesystem::path("/usr/share/icons/hicolor/256x256/apps")
                        : home_dir() / ".local/share/icons/hicolor/256x256/apps";
                    std::filesystem::create_directories(icon_root, ec);
                    auto dst = icon_root / "vesta.png";
                    std::filesystem::copy_file(icon_src, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        ManifestFile e; e.path = dst;
                        e.size = std::filesystem::file_size(dst, ec);
                        mf.files.push_back(e);
                    }
                }
            }
            return true;
        }

        // ---- asociaciones: MIME types + .desktop ----
        bool register_associations(const InstallOptions& opts, Manifest& mf) override {
            auto exe = opts.bin_dir / "vesta";

            // Carpetas XDG
            std::filesystem::path apps_dir = (geteuid() == 0)
                ? "/usr/share/applications"
                : home_dir() / ".local/share/applications";
            std::filesystem::path mime_dir = (geteuid() == 0)
                ? "/usr/share/mime/packages"
                : home_dir() / ".local/share/mime/packages";

            std::error_code ec;
            std::filesystem::create_directories(apps_dir, ec);
            std::filesystem::create_directories(mime_dir, ec);

            // Un MIME por extension
            for (auto& [ext, ao] : opts.associations) {
                if (!ao.enabled) continue;
                std::string ext_clean = ext.substr(1);  // sin punto
                std::string mime = "application/x-vesta-" + ext_clean;

                // [1] MIME type XML
                auto mime_path = mime_dir / ("vesta-" + ext_clean + ".xml");
                {
                    std::ofstream f(mime_path);
                    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
                    f << "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
                    f << "  <mime-type type=\"" << mime << "\">\n";
                    f << "    <comment>" << ao.description << "</comment>\n";
                    f << "    <glob pattern=\"*" << ext << "\"/>\n";
                    f << "  </mime-type>\n";
                    f << "</mime-info>\n";
                }
                mf.mime_files.push_back({ mime_path });

                // [2] .desktop entry
                auto desk_path = apps_dir / ("vesta-" + ext_clean + ".desktop");
                {
                    std::ofstream f(desk_path);
                    f << "[Desktop Entry]\n";
                    f << "Type=Application\n";
                    f << "Name=" << ao.description << "\n";
                    f << "GenericName=Vesta\n";
                    f << "Exec=" << exe.string();
                    if (!ao.command_template.empty()) {
                        // Convertir "%1" -> "%f" en sintaxis .desktop
                        std::string tmpl = ao.command_template;
                        size_t p = tmpl.find("\"%1\"");
                        if (p != std::string::npos) tmpl.replace(p, 4, "%f");
                        else if ((p = tmpl.find("%1")) != std::string::npos)
                            tmpl.replace(p, 2, "%f");
                        f << " " << tmpl;
                    } else {
                        f << " %f";
                    }
                    f << "\n";
                    f << "Icon=vesta\n";
                    f << "Terminal=true\n";
                    f << "MimeType=" << mime << ";\n";
                    f << "NoDisplay=true\n";   // no aparece en menus, solo asocia
                    f << "Categories=Development;\n";
                }
                mf.desktop_files.push_back({ desk_path });

                // [3] Asignar como handler default del MIME (per-user)
                if (geteuid() != 0) {
                    std::string cmd = "xdg-mime default vesta-" + ext_clean +
                                      ".desktop " + mime + " 2>/dev/null";
                    run_cmd(cmd);
                }
            }

            // Refrescar bases de datos XDG
            std::string upd_mime = (geteuid() == 0)
                ? "update-mime-database /usr/share/mime 2>/dev/null"
                : "update-mime-database " + (home_dir() / ".local/share/mime").string() +
                  " 2>/dev/null";
            std::string upd_desk = (geteuid() == 0)
                ? "update-desktop-database /usr/share/applications 2>/dev/null"
                : "update-desktop-database " + (home_dir() / ".local/share/applications").string() +
                  " 2>/dev/null";
            run_cmd(upd_mime);
            run_cmd(upd_desk);
            return true;
        }

        // ---- PATH: anadir bin_dir a ~/.bashrc / ~/.zshrc ----
        bool add_to_path(const InstallOptions& opts, Manifest& mf) override {
            std::string entry = opts.bin_dir.string();
            // System-wide: si el bin_dir es /usr/local/bin, ya esta en PATH default.
            if (geteuid() == 0 && entry == "/usr/local/bin") {
                mf.path_entries.push_back({ entry, "system" });
                return true;
            }

            // Per-user: insertar marcador en .bashrc y .zshrc si existen.
            const char* shells[] = { ".bashrc", ".zshrc", ".profile" };
            std::string marker_begin = "# >>> vesta install begin >>>";
            std::string marker_end   = "# <<< vesta install end <<<";
            std::string block =
                marker_begin + "\n" +
                "case \":${PATH}:\" in\n"
                "  *\":" + entry + ":\"*) ;;\n"
                "  *) export PATH=\"" + entry + ":${PATH}\" ;;\n"
                "esac\n" +
                marker_end + "\n";

            for (const char* sh : shells) {
                auto rc = home_dir() / sh;
                std::ifstream in(rc);
                std::stringstream buf;
                if (in) buf << in.rdbuf();
                std::string content = buf.str();
                if (content.find(marker_begin) != std::string::npos) continue; // ya esta
                std::ofstream out(rc, std::ios::app);
                if (!out) continue;
                out << "\n" << block;
            }
            mf.path_entries.push_back({ entry, "user" });
            return true;
        }

        // ---- shortcuts: .desktop principal del REPL ----
        bool create_shortcuts(const InstallOptions& opts, Manifest& mf) override {
            if (!opts.create_start_menu && !opts.create_desktop_shortcut) return true;

            auto exe = opts.bin_dir / "vesta";
            std::filesystem::path apps_dir = (geteuid() == 0)
                ? "/usr/share/applications"
                : home_dir() / ".local/share/applications";

            // Lanzador principal (visible en menu)
            auto desk = apps_dir / "vesta.desktop";
            {
                std::ofstream f(desk);
                f << "[Desktop Entry]\n"
                  << "Type=Application\n"
                  << "Name=Vesta VM\n"
                  << "GenericName=Vesta REPL\n"
                  << "Exec=" << exe.string() << "\n"
                  << "Icon=vesta\n"
                  << "Terminal=true\n"
                  << "Categories=Development;IDE;\n";
            }
            mf.shortcuts.push_back({ "applications", desk });

            if (opts.create_desktop_shortcut) {
                auto desktop = home_dir() / "Desktop" / "vesta.desktop";
                std::error_code ec;
                std::filesystem::create_directories(desktop.parent_path(), ec);
                std::filesystem::copy_file(desk, desktop,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::filesystem::permissions(desktop,
                        std::filesystem::perms::owner_all,
                        std::filesystem::perm_options::add, ec);
                    mf.shortcuts.push_back({ "desktop", desktop });
                }
            }
            return true;
        }

        // En Linux no hay equivalente a Add/Remove Programs; el manifest basta.
        bool register_uninstaller(const InstallOptions& /*opts*/, Manifest& /*mf*/) override {
            return true;
        }

        // ---- desinstalacion ----
        bool unregister_associations(const Manifest& mf) override {
            std::error_code ec;
            for (auto& d : mf.desktop_files) std::filesystem::remove(d.path, ec);
            for (auto& m : mf.mime_files)    std::filesystem::remove(m.path, ec);
            // Refrescar bases
            run_cmd("update-mime-database " +
                    (home_dir() / ".local/share/mime").string() + " 2>/dev/null");
            run_cmd("update-desktop-database " +
                    (home_dir() / ".local/share/applications").string() + " 2>/dev/null");
            return true;
        }

        bool remove_from_path(const Manifest& mf) override {
            std::string marker_begin = "# >>> vesta install begin >>>";
            std::string marker_end   = "# <<< vesta install end <<<";

            const char* shells[] = { ".bashrc", ".zshrc", ".profile" };
            for (const char* sh : shells) {
                auto rc = home_dir() / sh;
                std::ifstream in(rc);
                if (!in) continue;
                std::stringstream buf; buf << in.rdbuf();
                std::string content = buf.str();
                auto a = content.find(marker_begin);
                if (a == std::string::npos) continue;
                auto b = content.find(marker_end, a);
                if (b == std::string::npos) continue;
                b += marker_end.size();
                if (b < content.size() && content[b] == '\n') ++b;
                std::string trimmed = content.substr(0, a) + content.substr(b);
                std::ofstream out(rc, std::ios::trunc);
                out << trimmed;
            }
            (void)mf;
            return true;
        }

        bool remove_shortcuts(const Manifest& mf) override {
            std::error_code ec;
            for (auto& s : mf.shortcuts) std::filesystem::remove(s.path, ec);
            return true;
        }

        bool unregister_uninstaller(const Manifest& /*mf*/) override { return true; }

        bool remove_files(const Manifest& mf, bool /*keep_user_data*/) override {
            std::error_code ec;
            // Borrar symlinks primero
            for (auto& s : mf.symlinks) std::filesystem::remove(s.link, ec);
            // Borrar ficheros
            for (auto it = mf.files.rbegin(); it != mf.files.rend(); ++it)
                std::filesystem::remove(it->path, ec);
            std::filesystem::remove(mf.manifest_path(), ec);
            std::filesystem::remove(mf.prefix, ec);
            return true;
        }

        void notify_system() override {
            // Ya hicimos update-*-database; no hay mas que hacer.
        }
    };

    Platform& current_platform() {
        static PlatformLinux inst;
        return inst;
    }

} // namespace install

#endif // !_WIN32
