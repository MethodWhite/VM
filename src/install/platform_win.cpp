/*
 * VestaVM - src/install/platform_win.cpp
 *
 * Backend Windows del instalador. Reemplaza el install.bat original con
 * llamadas directas a la Win32 API:
 *   - Asociaciones via HKLM\Software\Classes (system) o HKCU\Software\Classes (user)
 *   - PATH via la clave Environment del usuario o HKLM\System\...\Environment
 *   - Add/Remove Programs via HKLM\...\Uninstall\VestaVM o HKCU equivalente
 *   - Notificacion al Explorer via SHChangeNotify y broadcast WM_SETTINGCHANGE
 */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winreg.h>

#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

#include "install/platform.h"
#include "install/manifest.h"
#include "install/install_options.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "User32.lib")

namespace install {

// =====================================================================
// PATCH para src/install/platform_win.cpp
// =====================================================================
// Reemplaza la definicion actual de reg_delete_tree (que depende de
// RegDeleteTreeA, no disponible si _WIN32_WINNT < 0x0600) por una
// implementacion recursiva propia que solo usa RegDeleteKeyA y
// RegEnumKeyExA, funciones presentes desde Windows 2000.
//
// Cambia tambien la firma para devolver bool consistentemente.
// =====================================================================

// LOCALIZAR el bloque actual:
//
//     /// Borra una clave entera (recursivamente).
//     static bool reg_delete_tree(HKEY root, const std::string& subkey) {
//         return RegDeleteTreeA(root, subkey.c_str()) == ERROR_SUCCESS;
//     }
//
// REEMPLAZAR por lo siguiente:

    /// Borra una clave entera, incluidas sus subclaves, recursivamente.
    /// Equivalente a RegDeleteTreeA pero compatible con _WIN32_WINNT < 0x0600.
    static bool reg_delete_tree(HKEY root, const std::string& subkey) {
        HKEY k = nullptr;
        // Abrimos con permisos suficientes para enumerar y borrar hijos
        LONG rc = RegOpenKeyExA(root, subkey.c_str(), 0,
                                KEY_READ | KEY_WRITE, &k);
        if (rc == ERROR_FILE_NOT_FOUND) return true;   // ya no existe
        if (rc != ERROR_SUCCESS) return false;

        // Enumerar y borrar subclaves. Hay que enumerar siempre el indice 0
        // porque al borrar el 0 el resto se reordena (todos suben una posicion).
        for (;;) {
            char name[MAX_PATH];
            DWORD name_len = MAX_PATH;
            FILETIME ft{};
            LONG er = RegEnumKeyExA(k, 0, name, &name_len,
                                    nullptr, nullptr, nullptr, &ft);
            if (er == ERROR_NO_MORE_ITEMS) break;
            if (er != ERROR_SUCCESS) { RegCloseKey(k); return false; }

            // Borrado recursivo de la subclave (path completo desde root)
            std::string child_path = subkey + "\\" + name;
            if (!reg_delete_tree(root, child_path)) {
                RegCloseKey(k);
                return false;
            }
        }
        RegCloseKey(k);

        // Una vez vacia, borrar la clave en si.
        rc = RegDeleteKeyA(root, subkey.c_str());
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }


    // ====================================================================
    // Helpers de registry
    // ====================================================================

    static HKEY hive_to_root(const std::string& hive) {
        if (hive == "HKLM") return HKEY_LOCAL_MACHINE;
        return HKEY_CURRENT_USER;
    }

    /// Crea (si no existe) la subclave indicada bajo @p root y la abre.
    static bool reg_create_key(HKEY root, const std::string& subkey, HKEY& out) {
        DWORD disp = 0;
        LONG  rc   = RegCreateKeyExA(root, subkey.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                     nullptr, &out, &disp);
        return rc == ERROR_SUCCESS;
    }

    /// Escribe REG_SZ con un nombre de valor (vacio = (Default)).
    static bool reg_set_sz(HKEY root, const std::string& subkey,
                           const std::string& value_name, const std::string& data)
    {
        HKEY k = nullptr;
        if (!reg_create_key(root, subkey, k)) return false;
        LONG rc = RegSetValueExA(k,
                                 value_name.empty() ? nullptr : value_name.c_str(),
                                 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(data.c_str()),
                                 (DWORD)data.size() + 1);
        RegCloseKey(k);
        return rc == ERROR_SUCCESS;
    }

    /// Escribe REG_EXPAND_SZ (para PATH y similares).
    static bool reg_set_expand_sz(HKEY root, const std::string& subkey,
                                  const std::string& value_name, const std::string& data)
    {
        HKEY k = nullptr;
        if (!reg_create_key(root, subkey, k)) return false;
        LONG rc = RegSetValueExA(k, value_name.c_str(), 0, REG_EXPAND_SZ,
                                 reinterpret_cast<const BYTE*>(data.c_str()),
                                 (DWORD)data.size() + 1);
        RegCloseKey(k);
        return rc == ERROR_SUCCESS;
    }

    /// Escribe REG_DWORD.
    static bool reg_set_dword(HKEY root, const std::string& subkey,
                              const std::string& value_name, DWORD data)
    {
        HKEY k = nullptr;
        if (!reg_create_key(root, subkey, k)) return false;
        LONG rc = RegSetValueExA(k, value_name.c_str(), 0, REG_DWORD,
                                 reinterpret_cast<const BYTE*>(&data), sizeof(data));
        RegCloseKey(k);
        return rc == ERROR_SUCCESS;
    }

    /// Lee REG_SZ / REG_EXPAND_SZ.
    static bool reg_get_sz(HKEY root, const std::string& subkey,
                           const std::string& value_name, std::string& out)
    {
        HKEY k = nullptr;
        if (RegOpenKeyExA(root, subkey.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
            return false;
        DWORD type = 0, len = 0;
        if (RegQueryValueExA(k, value_name.c_str(), nullptr, &type, nullptr, &len) != ERROR_SUCCESS) {
            RegCloseKey(k); return false;
        }
        if (type != REG_SZ && type != REG_EXPAND_SZ) { RegCloseKey(k); return false; }
        std::vector<char> buf(len + 1, 0);
        if (RegQueryValueExA(k, value_name.c_str(), nullptr, &type,
                             reinterpret_cast<BYTE*>(buf.data()), &len) != ERROR_SUCCESS) {
            RegCloseKey(k); return false;
        }
        RegCloseKey(k);
        out.assign(buf.data());
        return true;
    }

    /// Borra solo un valor.
    static bool reg_delete_value(HKEY root, const std::string& subkey,
                                  const std::string& value_name)
    {
        HKEY k = nullptr;
        if (RegOpenKeyExA(root, subkey.c_str(), 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS)
            return false;
        LONG rc = RegDeleteValueA(k, value_name.c_str());
        RegCloseKey(k);
        return rc == ERROR_SUCCESS;
    }

    // ====================================================================
    // Helpers varios
    // ====================================================================

    /// Localiza el binario actual.
    static std::filesystem::path module_path() {
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return std::filesystem::path(std::string(buf, n));
    }

    /// Expande %ProgramFiles%, %LOCALAPPDATA%, etc.
    static std::filesystem::path expand_env(const std::string& s) {
        char  buf[MAX_PATH];
        DWORD n = ExpandEnvironmentStringsA(s.c_str(), buf, MAX_PATH);
        return std::filesystem::path(std::string(buf, n - 1));
    }

    /// @brief @c true si la entrada es basura de build (cmake intermedios)
    /// que no debe instalarse.  Usado por @c copy_dir_recursive para
    /// filtrar @c stdlib/ y otras carpetas que el build de cmake llena
    /// con CMakeFiles/, libfoo.dll.a, *.cmake, etc.  Lo que SI se copia:
    /// .dll/.so/.dylib (plugins), .vex/.vsh/.vel/.velb (codigo runtime),
    /// .md/.txt si es @c doc/.  Lo que NO: directorios @c CMakeFiles,
    /// @c .git, @c __pycache__, archivos @c .a/.o/.obj/.lib/.exp/.cmake/.txt
    /// terminados en cmake artifacts.
    static bool is_build_garbage(const std::filesystem::path& p) {
        // Filtrar por nombre de directorio.
        for (const auto& part : p) {
            const std::string s = part.string();
            if (s == "CMakeFiles"
             || s == ".git"
             || s == ".vs"
             || s == "__pycache__") {
                return true;
            }
        }
        // Filtrar por extension/nombre del archivo.
        const std::string fn   = p.filename().string();
        const std::string ext  = p.extension().string();
        if (fn == "Makefile"
         || fn == "cmake_install.cmake"
         || fn == "CMakeCache.txt"
         || fn == "CTestTestfile.cmake") return true;
        // Import libraries de MinGW: lib<plugin>.dll.a son interfaz de
        // link, NO necesarias en runtime.
        if (fn.size() > 6 && fn.substr(fn.size() - 6) == ".dll.a") return true;
        // Otros artefactos de build sin valor en runtime.
        if (ext == ".obj" || ext == ".o"   || ext == ".lib"
         || ext == ".exp" || ext == ".pdb" || ext == ".ilk"
         || ext == ".d"   || ext == ".rsp" || ext == ".cmake") return true;
        // Intermediarios del frontend Vex (debug-only).  El usuario final
        // no necesita: .ir (SSA dump), .velb-map (debug map del linker).
        // Conservamos .vex (source), .vel (assembly), .velb (ejecutable).
        if (ext == ".ir") return true;
        if (fn.size() > 9 && fn.substr(fn.size() - 9) == ".velb-map") return true;
        return false;
    }

    static bool copy_dir_recursive(const std::filesystem::path& src,
                                    const std::filesystem::path& dst,
                                    Manifest& mf, bool overwrite)
    {
        std::error_code ec;
        std::filesystem::create_directories(dst, ec);
        for (auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
            if (ec) return false;
            // Saltar basura de build (CMakeFiles/, *.dll.a, etc.).
            // Aplica tanto a directorios (skip_recursion via continue + skip
            // de subarbol manual no necesario porque is_build_garbage
            // tambien matchea cualquier fichero dentro).
            if (is_build_garbage(entry.path())) continue;
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
                    ManifestFile mfent;
                    mfent.path = target;
                    mfent.size = std::filesystem::file_size(target, ec);
                    mf.files.push_back(mfent);
                }
            }
        }
        return true;
    }

    // ====================================================================
    // PlatformWin
    // ====================================================================

    class PlatformWin : public Platform {
    public:
        // ---- privilegios ----
        bool is_elevated() const override {
            BOOL  elev = FALSE;
            HANDLE tok = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
                TOKEN_ELEVATION te{};
                DWORD sz = 0;
                if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz))
                    elev = te.TokenIsElevated;
                CloseHandle(tok);
            }
            return elev != FALSE;
        }
        std::string elevation_hint() const override {
            return "Re-ejecuta este comando desde una consola \"Run as Administrator\".";
        }

        // ---- rutas ----
        std::filesystem::path default_prefix(Scope scope) const override {
            if (scope == Scope::SystemWide)
                return expand_env("%ProgramFiles%") / "VestaVM";
            return expand_env("%LOCALAPPDATA%") / "Programs" / "VestaVM";
        }
        std::filesystem::path default_bin_dir(const std::filesystem::path& p) const override {
            return p;   // En Windows el binario y los assets viven juntos
        }
        std::filesystem::path default_share_dir(const std::filesystem::path& p) const override {
            return p;
        }

        // ---- copia ----
        bool copy_files(const InstallOptions& opts, Manifest& mf) override {
            std::error_code ec;
            std::filesystem::create_directories(opts.prefix, ec);

            // 1) binario propio
            if (opts.copy_binary) {
                auto src = module_path();
                auto dst = opts.prefix / src.filename();
                std::filesystem::copy_file(src, dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "[copy] " << ec.message() << "\n"; return false;
                }
                ManifestFile e; e.path = dst;
                e.size = std::filesystem::file_size(dst, ec);
                mf.files.push_back(e);
            }

            // 2) assets relativos al binario actual: docs/, examples_codes_vsh/, icons/
            //
            // BUG fix: el binario puede estar en un build dir paralelo
            // (ej. cmake-vex-build/vm.exe) donde los assets de source no
            // existen.  Probamos varias raices y usamos la primera que
            // contenga la carpeta.  Orden: <exe_dir>, <exe_dir>/.., <exe_dir>/../..
            auto src_root = module_path().parent_path();

            auto try_copy_dir = [&](const char* name, bool enabled) {
                if (!enabled) return;
                const std::filesystem::path roots[] = {
                    src_root,
                    src_root.parent_path(),
                    src_root.parent_path().parent_path(),
                };
                for (const auto &r : roots) {
                    auto s = r / name;
                    if (std::filesystem::exists(s)) {
                        copy_dir_recursive(s, opts.prefix / name, mf, opts.force);
                        return;
                    }
                }
            };

            try_copy_dir("doc",                  opts.copy_docs);
            try_copy_dir("examples_codes_vsh",   opts.copy_examples);
            try_copy_dir("examples_codes_vm",    opts.copy_examples);
            try_copy_dir("examples_codes_vex",   opts.copy_examples);
            try_copy_dir("stdlib",               opts.copy_stdlib);
            try_copy_dir("icons",                opts.copy_icons);

            // ----------------------------------------------------------------
            // Fallback critico para plugins nativos (fix instalador):
            //
            // try_copy_dir("stdlib") ya copia recursivo lo que encuentre en
            // <src_root>/stdlib/.  Pero esa carpeta puede ser SOLO sources
            // (sin .dll) si el binario corre desde un build dir paralelo.
            // Garantizamos los plugins runtime esenciales (vesta_io.dll y
            // vesta_math.dll) buscandolos en lugares conocidos y copiandolos
            // a la ruta relativa esperada por el loader (LoadLibraryA usa
            // <exe_dir>/stdlib/native/<lib>/<name>.dll).
            //
            // Ubicaciones probadas en orden:
            //   1. <src_root>/stdlib/native/<lib>/<name>.dll  (build local)
            //   2. <src_root>/<name>.dll                       (junto a vm.exe)
            //   3. <src_root>/../stdlib/native/<lib>/<name>.dll (cmake parent)
            //   4. <src_root>/../../stdlib/native/<lib>/<name>.dll
            //
            // Solo se copia si el destino aun no existe (idempotencia).
            // ----------------------------------------------------------------
            if (opts.copy_stdlib) {
                struct PluginSpec { const char *lib; const char *file; };
                static const PluginSpec plugins[] = {
                    { "io",   "vesta_io.dll"   },
                    { "math", "vesta_math.dll" },
                };
                for (const auto &ps : plugins) {
                    const std::string rel_path =
                        std::string("stdlib/native/") + ps.lib + "/" + ps.file;
                    auto dst = opts.prefix / rel_path;
                    if (std::filesystem::exists(dst)) continue;  // ya copiado

                    // Probar varias ubicaciones.
                    const std::filesystem::path candidates[] = {
                        src_root / "stdlib" / "native" / ps.lib / ps.file,
                        src_root / ps.file,
                        src_root.parent_path() / "stdlib" / "native" / ps.lib / ps.file,
                        src_root.parent_path().parent_path() / "stdlib" / "native" / ps.lib / ps.file,
                    };
                    bool copied = false;
                    for (const auto &cand : candidates) {
                        if (!std::filesystem::exists(cand)) continue;
                        std::filesystem::create_directories(dst.parent_path(), ec);
                        std::filesystem::copy_file(cand, dst,
                            std::filesystem::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            ManifestFile e; e.path = dst;
                            e.size = std::filesystem::file_size(dst, ec);
                            mf.files.push_back(e);
                            std::cout << "  [plugin] " << ps.file
                                      << " <- " << cand.string() << "\n";
                            copied = true;
                        }
                        break;
                    }
                    if (!copied) {
                        std::cerr << "  [plugin] AVISO: " << ps.file
                                  << " no encontrado en ninguna ubicacion conocida; "
                                  << "los programas que lo requieran fallaran al cargar.\n";
                    }
                }
            }

            // 3) icono individual si existe en el directorio fuente
            if (opts.copy_icons) {
                auto icon = src_root / "icono.ico";
                if (std::filesystem::exists(icon)) {
                    auto dst = opts.prefix / "icono.ico";
                    std::filesystem::copy_file(icon, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        ManifestFile e; e.path = dst;
                        e.size = std::filesystem::file_size(dst, ec);
                        mf.files.push_back(e);
                    }
                }
            }

            // 4) extension de VS Code (copia si existe)
            if (opts.copy_vscode_ext) {
                auto src = src_root / "ext_code" / "vsh";
                if (std::filesystem::exists(src)) {
                    char buf[MAX_PATH];
                    DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
                    if (n > 0) {
                        std::filesystem::path home(std::string(buf, n));
                        auto vsdir = home / ".vscode" / "extensions" / "desmonhak.vesta-shell-0.1.0";
                        copy_dir_recursive(src, vsdir, mf, opts.force);
                    }
                }
            }
            return true;
        }

        // ---- asociaciones ----
        bool register_associations(const InstallOptions& opts, Manifest& mf) override {
            HKEY root = (opts.scope == Scope::SystemWide) ? HKEY_LOCAL_MACHINE
                                                          : HKEY_CURRENT_USER;
            std::string hive_name = (opts.scope == Scope::SystemWide) ? "HKLM" : "HKCU";
            auto exe  = opts.prefix / "vm.exe";
            auto icon = opts.prefix / "icono.ico";

            for (auto& [ext, ao] : opts.associations) {
                if (!ao.enabled) continue;

                // ProgID unico por extension: VestaVM.<ext sin punto>file
                std::string progid = "VestaVM." + ext.substr(1) + "file";

                // [1] Software\Classes\<.ext> = progid
                std::string base   = "Software\\Classes\\";
                std::string ext_key = base + ext;
                if (!reg_set_sz(root, ext_key, "", progid)) return false;
                mf.registry.push_back({ hive_name, ext_key, "", progid });

                // [2] Software\Classes\<progid> = descripcion
                std::string class_key = base + progid;
                if (!reg_set_sz(root, class_key, "", ao.description)) return false;
                mf.registry.push_back({ hive_name, class_key, "", ao.description });

                // [3] Software\Classes\<progid>\DefaultIcon = "<icon>"
                if (ao.create_icon && std::filesystem::exists(icon)) {
                    std::string ikey = class_key + "\\DefaultIcon";
                    std::string idata = "\"" + icon.string() + "\"";
                    if (!reg_set_sz(root, ikey, "", idata)) return false;
                    mf.registry.push_back({ hive_name, ikey, "", idata });
                }

                // [4] Software\Classes\<progid>\shell\<verb>\command = "<exe>" args
                if (!ao.command_template.empty()) {
                    std::string ckey = class_key + "\\shell\\" + ao.verb_open + "\\command";
                    std::string cdata = "\"" + exe.string() + "\" " + ao.command_template;
                    if (!reg_set_sz(root, ckey, "", cdata)) return false;
                    mf.registry.push_back({ hive_name, ckey, "", cdata });
                }
            }
            return true;
        }

        // ---- PATH ----
        bool add_to_path(const InstallOptions& opts, Manifest& mf) override {
            HKEY root;
            std::string subkey;
            std::string scope_name;
            if (opts.scope == Scope::SystemWide) {
                root = HKEY_LOCAL_MACHINE;
                subkey = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
                scope_name = "system";
            } else {
                root = HKEY_CURRENT_USER;
                subkey = "Environment";
                scope_name = "user";
            }

            std::string current;
            reg_get_sz(root, subkey, "Path", current);

            std::string entry = opts.bin_dir.string();
            if (current.find(entry) != std::string::npos) {
                // Ya estaba; no duplicar pero si registrar para idempotencia.
                mf.path_entries.push_back({ entry, scope_name });
                return true;
            }

            std::string new_path = current.empty() ? entry : current + ";" + entry;
            if (!reg_set_expand_sz(root, subkey, "Path", new_path)) return false;
            mf.path_entries.push_back({ entry, scope_name });
            return true;
        }

        // ---- shortcuts ----
        bool create_shortcuts(const InstallOptions& opts, Manifest& mf) override {
            // Crear .lnk via IShellLink/IPersistFile
            CoInitialize(nullptr);
            auto make_lnk = [&](const std::filesystem::path& lnk_path,
                                const std::filesystem::path& target,
                                const std::string& args,
                                const std::string& desc) -> bool {
                IShellLinkA* sl = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_IShellLinkA, (void**)&sl);
                if (FAILED(hr)) return false;
                sl->SetPath(target.string().c_str());
                if (!args.empty()) sl->SetArguments(args.c_str());
                sl->SetWorkingDirectory(target.parent_path().string().c_str());
                if (!desc.empty()) sl->SetDescription(desc.c_str());
                auto icon = opts.prefix / "icono.ico";
                if (std::filesystem::exists(icon)) sl->SetIconLocation(icon.string().c_str(), 0);

                IPersistFile* pf = nullptr;
                hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
                bool ok = false;
                if (SUCCEEDED(hr)) {
                    // Convertir la ruta UTF-8 a UTF-16 correctamente con la API Win32.
                    // No vale `std::wstring(s.begin(), s.end())`: eso solo promueve
                    // bytes a wchar_t y rompe ante cualquier caracter no-ASCII; ademas
                    // el patron `path.string().begin() ... path.string().end()` da UB
                    // porque cada llamada a string() produce un temporal distinto.
                    std::string  narrow = lnk_path.string();
                    std::wstring wp;
                    int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                                   narrow.c_str(), (int)narrow.size(),
                                                   nullptr, 0);
                    if (wlen > 0) {
                        wp.resize((size_t)wlen);
                        MultiByteToWideChar(CP_UTF8, 0,
                                            narrow.c_str(), (int)narrow.size(),
                                            wp.data(), wlen);
                    }
                    hr = pf->Save(wp.c_str(), TRUE);
                    ok = SUCCEEDED(hr);
                    pf->Release();
                }
                sl->Release();
                return ok;
            };

            auto exe = opts.prefix / "vm.exe";

            if (opts.create_start_menu) {
                // Carpeta del menu inicio
                int csidl = (opts.scope == Scope::SystemWide)
                          ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS;
                char buf[MAX_PATH];
                if (SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf) == S_OK) {
                    std::filesystem::path dir = std::filesystem::path(buf) / "Vesta VM";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    auto lnk = dir / "Vesta REPL.lnk";
                    if (make_lnk(lnk, exe, "", "Vesta VM REPL")) {
                        mf.shortcuts.push_back({ "start_menu", lnk });
                    }
                    auto lnk2 = dir / "Vesta Shell.lnk";
                    if (make_lnk(lnk2, exe, "--interprete", "Vesta Shell REPL")) {
                        mf.shortcuts.push_back({ "start_menu", lnk2 });
                    }
                }
            }
            if (opts.create_desktop_shortcut) {
                int csidl = (opts.scope == Scope::SystemWide)
                          ? CSIDL_COMMON_DESKTOPDIRECTORY : CSIDL_DESKTOPDIRECTORY;
                char buf[MAX_PATH];
                if (SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf) == S_OK) {
                    std::filesystem::path lnk = std::filesystem::path(buf) / "Vesta VM.lnk";
                    if (make_lnk(lnk, exe, "", "Vesta VM")) {
                        mf.shortcuts.push_back({ "desktop", lnk });
                    }
                }
            }
            CoUninitialize();
            return true;
        }

        // ---- desinstalador en Add/Remove Programs ----
        bool register_uninstaller(const InstallOptions& opts, Manifest& mf) override {
            HKEY root = (opts.scope == Scope::SystemWide) ? HKEY_LOCAL_MACHINE
                                                          : HKEY_CURRENT_USER;
            std::string hive_name = (opts.scope == Scope::SystemWide) ? "HKLM" : "HKCU";
            std::string key = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VestaVM";
            auto exe = opts.prefix / "vm.exe";
            std::string uninst = "\"" + exe.string() + "\" --uninstall --silent";

            reg_set_sz(root, key, "DisplayName",     "Vesta VM");
            mf.registry.push_back({ hive_name, key, "DisplayName", "Vesta VM" });

            reg_set_sz(root, key, "DisplayVersion",  mf.vesta_version);
            mf.registry.push_back({ hive_name, key, "DisplayVersion", mf.vesta_version });

            reg_set_sz(root, key, "Publisher",       "DesmonHak");
            mf.registry.push_back({ hive_name, key, "Publisher", "DesmonHak" });

            reg_set_sz(root, key, "InstallLocation", opts.prefix.string());
            mf.registry.push_back({ hive_name, key, "InstallLocation", opts.prefix.string() });

            reg_set_sz(root, key, "UninstallString", uninst);
            mf.registry.push_back({ hive_name, key, "UninstallString", uninst });

            auto icon = opts.prefix / "icono.ico";
            if (std::filesystem::exists(icon)) {
                reg_set_sz(root, key, "DisplayIcon", icon.string());
                mf.registry.push_back({ hive_name, key, "DisplayIcon", icon.string() });
            }
            reg_set_dword(root, key, "NoModify", 1);
            reg_set_dword(root, key, "NoRepair", 1);
            return true;
        }

        // ---- desinstalacion ----
        bool unregister_associations(const Manifest& mf) override {
            // Borrar las claves registradas. Vamos en orden inverso para
            // limpiar las hojas antes que las raices.
            // Estrategia: borrar las claves de progid completas (subarbol).
            for (auto it = mf.registry.rbegin(); it != mf.registry.rend(); ++it) {
                HKEY r = hive_to_root(it->hive);
                // Si es la entrada de la extension (.vsh -> progid) o un progid
                // sin slash adicional, borramos el arbol entero.
                if (it->key.find("Software\\Classes\\.") != std::string::npos ||
                    it->key.find("VestaVM.") != std::string::npos) {
                    reg_delete_tree(r, it->key);
                }
            }
            return true;
        }

        bool remove_from_path(const Manifest& mf) override {
            for (auto& e : mf.path_entries) {
                HKEY root;
                std::string subkey;
                if (e.scope == "system") {
                    root = HKEY_LOCAL_MACHINE;
                    subkey = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
                } else {
                    root = HKEY_CURRENT_USER;
                    subkey = "Environment";
                }
                std::string current;
                if (!reg_get_sz(root, subkey, "Path", current)) continue;

                // Quitar la entrada (con o sin punto y coma circundante).
                auto pos = current.find(e.entry);
                if (pos == std::string::npos) continue;
                size_t end = pos + e.entry.size();
                // tragar el ; siguiente o el ; anterior
                if (end < current.size() && current[end] == ';') ++end;
                else if (pos > 0 && current[pos - 1] == ';') --pos;
                std::string new_path = current.substr(0, pos) + current.substr(end);
                reg_set_expand_sz(root, subkey, "Path", new_path);
            }
            return true;
        }

        bool remove_shortcuts(const Manifest& mf) override {
            std::error_code ec;
            for (auto& s : mf.shortcuts) {
                std::filesystem::remove(s.path, ec);
                // Si la carpeta del menu queda vacia, borrarla
                std::filesystem::remove(s.path.parent_path(), ec);
            }
            return true;
        }

        bool unregister_uninstaller(const Manifest& mf) override {
            for (auto& r : mf.registry) {
                if (r.key.find("\\Uninstall\\VestaVM") != std::string::npos) {
                    reg_delete_tree(hive_to_root(r.hive), r.key);
                    return true;
                }
            }
            return true;
        }

        bool remove_files(const Manifest& mf, bool /*keep_user_data*/) override {
            std::error_code ec;
            for (auto it = mf.files.rbegin(); it != mf.files.rend(); ++it) {
                std::filesystem::remove(it->path, ec);
            }
            // Borrar el manifest mismo y el directorio prefix si queda vacio
            std::filesystem::remove(mf.manifest_path(), ec);
            std::filesystem::remove(mf.prefix, ec);   // solo si esta vacio
            return true;
        }

        // ---- notificacion al sistema ----
        void notify_system() override {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
            DWORD_PTR result = 0;
            SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, &result);
        }
    };

    // ---- factory ----
    Platform& current_platform() {
        static PlatformWin inst;
        return inst;
    }

} // namespace install

#endif // _WIN32
