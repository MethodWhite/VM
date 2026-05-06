/*
 * VestaVM - src/install/install.cpp
 *
 * Orquestador del instalador. Maneja:
 *   - Parseo de flags y subcomandos
 *   - Wizard interactivo con edicion de opciones
 *   - Llamadas al backend de plataforma
 *   - Persistencia del manifest
 *   - Rollback parcial si algun paso falla
 */

#include "install/install.h"
#include "install/platform.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <cstring>

#if defined(_WIN32)
  #include <io.h>
  #define ISATTY _isatty
  #define FILENO _fileno
#else
  #include <unistd.h>
  #define ISATTY isatty
  #define FILENO fileno
#endif

namespace install {

    // ====================================================================
    // Helpers
    // ====================================================================

    static bool stdin_is_tty() {
        return ISATTY(FILENO(stdin)) != 0;
    }

    static std::string ask(const std::string& prompt, const std::string& def = "") {
        std::cout << prompt;
        if (!def.empty()) std::cout << " [" << def << "]";
        std::cout << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return def;
        if (line.empty()) return def;
        return line;
    }

    static bool ask_yn(const std::string& prompt, bool def_yes) {
        std::string def = def_yes ? "Y/n" : "y/N";
        std::cout << prompt << " [" << def << "]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return def_yes;
        if (line.empty()) return def_yes;
        char c = std::tolower((unsigned char)line[0]);
        return c == 'y' || c == 's';   // si o yes
    }

    static std::string current_iso_utc() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return os.str();
    }

    // ====================================================================
    // Wizard interactivo con edicion en bucle
    // ====================================================================

    /**
     * @brief Sub-prompt para editar una opcion concreta.
     *
     * Se llama cuando el usuario pulsa el numero/letra de una opcion en
     * el menu principal. Devuelve true si hubo cambio.
     */
    static bool edit_option(InstallOptions& opts, const std::string& key) {
        if (key == "1") {
            std::cout << "\n  1) Per-user (no requiere admin)\n";
            std::cout << "  2) System-wide (requiere admin/root)\n";
            std::string r = ask("  Eleccion", opts.scope == Scope::PerUser ? "1" : "2");
            Scope new_scope = (r == "2") ? Scope::SystemWide : Scope::PerUser;
            if (new_scope != opts.scope) {
                opts.scope  = new_scope;
                opts.prefix.clear();    // forzar recalculo
                opts.bin_dir.clear();
                opts.share_dir.clear();
                opts.resolve_paths();
            }
            return true;
        }
        if (key == "2") {
            std::string r = ask("  Nuevo prefix", opts.prefix.string());
            if (!r.empty()) opts.prefix = r;
            opts.bin_dir.clear();
            opts.share_dir.clear();
            opts.resolve_paths();
            return true;
        }
        if (key == "3") { opts.associations[".velb"].enabled = ask_yn("  Asociar .velb?",
                                opts.associations[".velb"].enabled); return true; }
        if (key == "4") { opts.associations[".vsh" ].enabled = ask_yn("  Asociar .vsh?",
                                opts.associations[".vsh"].enabled);  return true; }
        if (key == "5") { opts.associations[".vel" ].enabled = ask_yn("  Asociar .vel?",
                                opts.associations[".vel"].enabled);  return true; }
        if (key == "6") { opts.add_to_path = ask_yn("  Anadir al PATH?", opts.add_to_path);   return true; }
        if (key == "7") { opts.create_desktop_shortcut = ask_yn("  Acceso directo en escritorio?",
                                opts.create_desktop_shortcut); return true; }
        if (key == "8") { opts.create_start_menu = ask_yn("  Crear entrada en menu inicio?",
                                opts.create_start_menu); return true; }
        if (key == "9") { opts.register_uninstaller = ask_yn("  Registrar en Add/Remove Programs?",
                                opts.register_uninstaller); return true; }
        if (key == "a") { opts.copy_examples   = ask_yn("  Copiar ejemplos?",       opts.copy_examples);   return true; }
        if (key == "b") { opts.copy_docs       = ask_yn("  Copiar documentacion?",  opts.copy_docs);       return true; }
        if (key == "c") { opts.copy_vscode_ext = ask_yn("  Instalar extension VS Code?",
                                opts.copy_vscode_ext); return true; }
        std::cout << "  Opcion no reconocida.\n";
        return false;
    }

    /**
     * @brief Bucle principal del wizard. Devuelve true si el usuario
     *        confirma instalar; false si cancela.
     */
    static bool run_wizard(InstallOptions& opts) {
        std::cout << "\n";
        std::cout << " ============================================================\n";
        std::cout << "   Vesta VM Installer\n";
        std::cout << " ============================================================\n";

        Platform& plat = current_platform();
        if (opts.scope == Scope::SystemWide && !plat.is_elevated()) {
            std::cout << "  Has elegido instalacion system-wide pero no tienes "
                         "privilegios elevados.\n  " << plat.elevation_hint() << "\n";
            std::cout << "  Cambiando a per-user.\n";
            opts.scope = Scope::PerUser;
            opts.prefix.clear();
            opts.resolve_paths();
        }

        for (;;) {
            opts.print_summary();
            std::cout << "  > " << std::flush;
            std::string in;
            if (!std::getline(std::cin, in)) return false;
            if (in.empty()) continue;
            std::transform(in.begin(), in.end(), in.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            if (in == "q" || in == "quit" || in == "n") return false;
            if (in == "i" || in == "install" || in == "y") return true;
            edit_option(opts, in);
        }
    }

    // ====================================================================
    // execute()
    // ====================================================================

    int execute(InstallOptions opts) {
        Platform& plat = current_platform();
        opts.resolve_paths();

        // Si silent o no es tty, no abrimos wizard.
        bool interactive = !opts.silent && stdin_is_tty();
        if (interactive) {
            if (!run_wizard(opts)) {
                std::cout << "Instalacion cancelada.\n";
                return 1;
            }
        }

        // Validacion final
        if (opts.scope == Scope::SystemWide && !plat.is_elevated()) {
            std::cerr << "Error: instalacion system-wide requiere privilegios elevados.\n";
            std::cerr << plat.elevation_hint() << "\n";
            return 2;
        }

        Manifest mf;
        mf.vesta_version = "0.1.0";   // TODO: leer de un VESTA_VERSION
        mf.install_date  = current_iso_utc();
#if defined(_WIN32)
        mf.platform = "windows";
#else
        mf.platform = "linux";
#endif
        mf.scope  = (opts.scope == Scope::PerUser ? "user" : "system");
        mf.prefix = opts.prefix;

        // Pipeline de instalacion. Si algo falla, lo registrado en mf
        // permite hacer rollback parcial.
        auto step = [&](const char* name, bool ok) -> bool {
            std::cout << "[" << (ok ? " OK " : "FAIL") << "] " << name << "\n";
            return ok;
        };

        bool all_ok = true;
        if (opts.copy_binary || opts.copy_examples || opts.copy_docs ||
            opts.copy_icons  || opts.copy_stdlib   || opts.copy_vscode_ext) {
            all_ok &= step("Copiando ficheros",       plat.copy_files(opts, mf));
        }

        bool any_assoc = false;
        for (auto& [ext, a] : opts.associations) if (a.enabled) any_assoc = true;
        if (any_assoc) {
            all_ok &= step("Registrando asociaciones", plat.register_associations(opts, mf));
        }
        if (opts.add_to_path) {
            all_ok &= step("Anadiendo al PATH",        plat.add_to_path(opts, mf));
        }
        if (opts.create_start_menu || opts.create_desktop_shortcut) {
            all_ok &= step("Creando accesos directos", plat.create_shortcuts(opts, mf));
        }
        if (opts.register_uninstaller) {
            all_ok &= step("Registrando desinstalador", plat.register_uninstaller(opts, mf));
        }

        // Persistir manifest *despues* de cada paso seria mas seguro;
        // hacerlo solo al final es mas simple y suele bastar.
        if (!mf.save()) {
            std::cerr << "Aviso: no se pudo guardar el manifest. "
                         "El desinstalador necesitara la ruta manual.\n";
        }

        plat.notify_system();

        if (all_ok) {
            std::cout << "\nInstalacion completada en: " << opts.prefix.string() << "\n";
            std::cout << "Manifest: " << mf.manifest_path().string() << "\n";
            return 0;
        } else {
            std::cerr << "\nLa instalacion termino con errores. "
                         "Usa 'vesta uninstall' para revertir.\n";
            return 3;
        }
    }

    // ====================================================================
    // uninstall()
    // ====================================================================

    int uninstall(const std::filesystem::path& manifest_path,
                  bool keep_user_data, bool silent) {
        Manifest mf;
        if (!mf.load_from(manifest_path)) {
            std::cerr << "Error: no se puede leer manifest: "
                      << manifest_path.string() << "\n";
            return 1;
        }

        if (!silent && stdin_is_tty()) {
            std::cout << "Se va a desinstalar Vesta de: " << mf.prefix.string() << "\n";
            if (!ask_yn("Continuar?", false)) {
                std::cout << "Cancelado.\n";
                return 1;
            }
        }

        Platform& plat = current_platform();
        bool ok = true;
        ok &= plat.unregister_associations(mf);
        ok &= plat.remove_from_path(mf);
        ok &= plat.remove_shortcuts(mf);
        ok &= plat.unregister_uninstaller(mf);
        ok &= plat.remove_files(mf, keep_user_data);
        plat.notify_system();

        if (ok) {
            std::cout << "Vesta desinstalado correctamente.\n";
            return 0;
        }
        std::cerr << "Desinstalacion completada con avisos.\n";
        return 2;
    }

    // ====================================================================
    // repair()
    // ====================================================================

    int repair(const std::filesystem::path& manifest_path, bool silent) {
        Manifest mf;
        if (!mf.load_from(manifest_path)) {
            std::cerr << "Error: no se puede leer manifest: "
                      << manifest_path.string() << "\n";
            return 1;
        }
        // Reconstruir InstallOptions a partir del manifest seria mas robusto;
        // por ahora simplemente reasignamos asociaciones y PATH.
        InstallOptions opts;
        opts.silent = silent;
        opts.prefix = mf.prefix;
        opts.scope  = (mf.scope == "user" ? Scope::PerUser : Scope::SystemWide);
        opts.resolve_paths();

        Platform& plat = current_platform();
        Manifest  new_mf;
        new_mf.prefix    = mf.prefix;
        new_mf.scope     = mf.scope;
        new_mf.platform  = mf.platform;
        new_mf.install_date = current_iso_utc();
        new_mf.vesta_version = mf.vesta_version;

        bool ok = true;
        ok &= plat.register_associations(opts, new_mf);
        if (opts.add_to_path) ok &= plat.add_to_path(opts, new_mf);
        plat.notify_system();
        new_mf.save();

        return ok ? 0 : 2;
    }

    // ====================================================================
    // Parseo de flags ligeros (sin cxxopts: nos llega ya filtrado por main)
    // ====================================================================

    static bool starts_with(const std::string& s, const char* p) {
        return s.compare(0, std::strlen(p), p) == 0;
    }

    static InstallOptions parse_flags(const std::vector<std::string>& args,
                                       std::string& mode,
                                       std::filesystem::path& manifest_path)
    {
        InstallOptions opts;
        mode = "install";
        for (auto& a : args) {
            if      (a == "install")          mode = "install";
            else if (a == "uninstall")        mode = "uninstall";
            else if (a == "repair")           mode = "repair";
            else if (a == "--silent")         opts.silent  = true;
            else if (a == "--dry-run")        opts.dry_run = true;
            else if (a == "--force")          opts.force   = true;
            else if (a == "--verbose")        opts.verbose = true;
            else if (a == "--no-color")       opts.no_color = true;
            else if (a == "--per-user")       opts.scope   = Scope::PerUser;
            else if (a == "--system-wide")    opts.scope   = Scope::SystemWide;
            else if (a == "--no-path")        opts.add_to_path = false;
            else if (a == "--no-start-menu")  opts.create_start_menu = false;
            else if (a == "--no-uninstaller-entry") opts.register_uninstaller = false;
            else if (starts_with(a, "--prefix=")) opts.prefix = a.substr(9);
            else if (starts_with(a, "--manifest=")) manifest_path = a.substr(11);
            else if (starts_with(a, "--assoc=")) {
                // Solo activar las listadas, desactivar el resto.
                for (auto& [ext, ao] : opts.associations) ao.enabled = false;
                std::string list = a.substr(8);
                std::stringstream ss(list);
                std::string ext;
                while (std::getline(ss, ext, ',')) {
                    if (!ext.empty() && ext[0] != '.') ext = "." + ext;
                    if (opts.associations.count(ext))
                        opts.associations[ext].enabled = true;
                }
            }
            else if (starts_with(a, "--no-assoc=")) {
                std::string list = a.substr(11);
                std::stringstream ss(list);
                std::string ext;
                while (std::getline(ss, ext, ',')) {
                    if (!ext.empty() && ext[0] != '.') ext = "." + ext;
                    if (opts.associations.count(ext))
                        opts.associations[ext].enabled = false;
                }
            }
        }
        return opts;
    }

    int run_install_cli(int argc, char** argv) {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        std::string mode;
        std::filesystem::path mf_path;
        InstallOptions opts = parse_flags(args, mode, mf_path);

        if (mode == "uninstall") {
            if (mf_path.empty()) {
                Manifest fake;
                opts.resolve_paths();
                fake.prefix = opts.prefix;
                mf_path = fake.manifest_path();
            }
            return uninstall(mf_path, true, opts.silent);
        }
        if (mode == "repair") {
            if (mf_path.empty()) {
                Manifest fake;
                opts.resolve_paths();
                fake.prefix = opts.prefix;
                mf_path = fake.manifest_path();
            }
            return repair(mf_path, opts.silent);
        }
        return execute(opts);
    }

    void run_install_repl(const std::string& args_str) {
        std::vector<std::string> argv_holder;
        std::stringstream ss(args_str);
        std::string tok;
        argv_holder.push_back("install");
        while (ss >> tok) argv_holder.push_back(tok);

        std::vector<char*> argv;
        for (auto& s : argv_holder) argv.push_back(s.data());
        argv.push_back(nullptr);
        // El primer arg "install" es el subcomando, pasamos directamente.
        std::string mode;
        std::filesystem::path mf;
        InstallOptions opts = parse_flags(
            std::vector<std::string>(argv_holder.begin(), argv_holder.end()),
            mode, mf);
        execute(opts);
    }

    void run_uninstall_repl(const std::string& args_str) {
        std::vector<std::string> argv_holder;
        std::stringstream ss(args_str);
        std::string tok;
        argv_holder.push_back("uninstall");
        while (ss >> tok) argv_holder.push_back(tok);

        std::string mode;
        std::filesystem::path mf;
        InstallOptions opts = parse_flags(argv_holder, mode, mf);
        if (mf.empty()) {
            opts.resolve_paths();
            Manifest fake; fake.prefix = opts.prefix;
            mf = fake.manifest_path();
        }
        uninstall(mf, true, opts.silent);
    }

} // namespace install
