/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file fs_utils.h
 * @brief Utilidades portables para comprobacion de rutas y permisos (Windows / POSIX).
 *
 * Este fichero contiene utilidades ligeras basadas en std::filesystem para:
 *   - Comprobar existencia de rutas (archivo o directorio).
 *   - Normalizar rutas a forma absoluta o canonica.
 *   - Comprobar permisos de lectura/escritura (heuristico en Windows).
 *   - Buscar ejecutables en el PATH del sistema.
 *   - Obtener la ruta del ejecutable actual.
 *
 * @note Requiere compilador con soporte para <filesystem> (C++17 o posterior).
 */

#ifndef FS_UTILS_H
#define FS_UTILS_H

#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include "windows.h"
#else
#include <unistd.h>
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace fs {
    /// Alias al namespace de la libreria estandar para evitar colisiones de nombre.
    namespace fs = std::filesystem;

    /**
     * @brief Comprueba si una ruta existe (archivo o directorio).
     *
     * @param p Ruta a comprobar.
     * @return true si la ruta existe; false en caso contrario o si ocurre un error.
     */
    static bool file_exists(const fs::path &p) {
        std::error_code ec;
        return fs::exists(p, ec) && !ec;
    }

    /**
     * @brief Comprueba si una ruta existe y es un archivo regular (no directorio).
     *
     * @param p Ruta a comprobar.
     * @return true si existe y es archivo regular; false en caso contrario.
     */
    static bool is_regular_file(const fs::path &p) {
        std::error_code ec;
        return fs::exists(p, ec) && !ec && fs::is_regular_file(p, ec) && !ec;
    }

    /**
     * @brief Divide una cadena tipo PATH por el separador de plataforma.
     *
     * En Windows el separador es ';' y en POSIX ':'.
     *
     * @param s Cadena PATH a dividir.
     * @return Vector con cada entrada del PATH (sin entradas vacias).
     */
    static std::vector<std::string> split_path_env(const std::string &s) {
        char sep =
#ifdef _WIN32
                ';';
#else
                ':';
#endif
        std::vector<std::string> out;
        std::string cur;
        for (char c: s) {
            if (c == sep) {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }


    /**
     * @brief Normaliza una ruta y la convierte a forma absoluta cuando es posible.
     *
     * Usa fs::weakly_canonical para resolver '.' y '..'. Si la ruta no existe
     * o weakly_canonical lanza, se devuelve una ruta lexically_normal relativa
     * al directorio actual.
     *
     * @param p Ruta a normalizar.
     * @return Ruta normalizada (absoluta si es posible).
     */
    static fs::path normalize_path_safe(const fs::path &p) {
        try {
            if (p.is_absolute()) return fs::weakly_canonical(p);
            return fs::weakly_canonical(fs::current_path() / p);
        } catch (const fs::filesystem_error &) {
            // weakly_canonical puede fallar si la ruta no existe; devolver una forma razonable
            return (fs::current_path() / p).lexically_normal();
        }
    }

    /**
     * @brief Comprueba de forma tentativa si el archivo es legible.
     *
     * En POSIX se comprueban los bits de permiso; en Windows se intenta abrir el
     * fichero en modo lectura (heuristica).
     *
     * @param p Ruta del fichero.
     * @return true si parece legible; false en caso contrario.
     */
    static bool can_read(const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) return false;
#ifdef _WIN32
        // Intentamos abrir en modo lectura sin lanzar excepcion
        std::ifstream f(p.string(), std::ios::binary);
        return f.is_open();
#else
        auto perms = fs::status(p, ec).permissions();
        if (ec) return false;
        using perms_t = fs::perms;
        // owner/group/others read
        return (perms & perms_t::owner_read) != perms_t::none
               || (perms & perms_t::group_read) != perms_t::none
               || (perms & perms_t::others_read) != perms_t::none;
#endif
    }

    /**
     * @brief Comprueba de forma tentativa si se puede escribir en la ruta.
     *
     * - Si el fichero no existe, se comprueba si el directorio padre es escribible.
     * - En Windows se intenta crear y borrar un fichero temporal como heuristica.
     *
     * @param p Ruta del fichero a comprobar.
     * @return true si parece escribible; false en caso contrario.
     */
    static bool can_write(const fs::path &p) {
        std::error_code ec;
        // Si no existe, comprobar si el directorio padre es escribible
        if (!fs::exists(p, ec) || ec) {
            fs::path parent = p.parent_path();
            if (parent.empty()) parent = fs::current_path();
#ifdef _WIN32
            // heuristica: intentar crear y borrar un fichero temporal
            fs::path tmp = parent / ".vesta_tmp_write_test";
            std::ofstream f(tmp.string(), std::ios::binary);
            if (!f.is_open()) return false;
            f.close();
            std::error_code rem;
            fs::remove(tmp, rem);
            return true;
#else
            auto perms = fs::status(parent, ec).permissions();
            if (ec) return false;
            using perms_t = fs::perms;
            return (perms & perms_t::owner_write) != perms_t::none
                   || (perms & perms_t::group_write) != perms_t::none
                   || (perms & perms_t::others_write) != perms_t::none;
#endif
        }
        // Si existe, comprobar bits/abrir para escritura
#ifdef _WIN32
        std::ofstream f(p.string(), std::ios::app | std::ios::binary);
        return f.is_open();
#else
        auto perms = fs::status(p, ec).permissions();
        if (ec) return false;
        using perms_t = fs::perms;
        return (perms & perms_t::owner_write) != perms_t::none
               || (perms & perms_t::group_write) != perms_t::none
               || (perms & perms_t::others_write) != perms_t::none;
#endif
    }

    /**
     * @brief Helper: comprobar una ruta dada por string (absoluta o relativa).
     *
     * Normaliza la ruta y comprueba existencia.
     *
     * @param s Ruta en forma de cadena.
     * @return true si el archivo existe.
     */
    static bool file_exists_str(const std::string &s) {
        return file_exists(normalize_path_safe(fs::path(s)));
    }

    /**
     * @brief Comprueba si la ruta existe y es ejecutable (heuristica multiplataforma).
     *
     * - En POSIX se comprueban los bits de ejecucion.
     * - En Windows se comprueba la extension contra PATHEXT.
     *
     * @param p Ruta a comprobar.
     * @return true si el fichero existe y parece ejecutable.
     */
    static bool path_exists_and_executable(const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) return false;
        if (fs::is_directory(p, ec) || ec) return false;

#ifdef _WIN32
        // En Windows: si existe el fichero y su extension esta en PATHEXT, considerarlo ejecutable.
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);

        const char *pathext_c = std::getenv("PATHEXT");
        if (!pathext_c) {
            // valor por defecto razonable
            static const std::vector<std::string> default_ext = {".COM", ".EXE", ".BAT", ".CMD"};
            for (auto &e: default_ext) if (e == ext) return true;
            return false;
        }
        std::string pathext(pathext_c);
        std::transform(pathext.begin(), pathext.end(), pathext.begin(), ::toupper);
        auto parts = split_path_env(pathext); // reutiliza split (usa ; en Windows)
        for (auto &pe: parts) {
            if (pe == ext) return true;
        }
        return false;
#else
        // POSIX: comprobar bits de ejecucion (owner/group/others)
        fs::perms pr = fs::status(p, ec).permissions();
        if (ec) return false;
        using perms = fs::perms;
        if ((pr & perms::owner_exec) != perms::none) return true;
        if ((pr & perms::group_exec) != perms::none) return true;
        if ((pr & perms::others_exec) != perms::none) return true;
        return false;
#endif
    }

    /**
     * @brief Comprueba si una cadena contiene separador de directorios.
     *
     * Util para decidir si tratar la cadena como ruta o como nombre simple.
     *
     * @param s Cadena a comprobar.
     * @return true si contiene separador de directorios.
     */
    static bool contains_dir_separator(const std::string &s) {
#ifdef _WIN32
        return s.find('\\') != std::string::npos || s.find('/') != std::string::npos;
#else
        return s.find('/') != std::string::npos;
#endif
    }

    /**
     * @brief Busca un ejecutable en PATH o comprueba la ruta si se paso una.
     *
     * @param cmd Nombre o ruta del ejecutable.
     * @return Ruta absoluta encontrada o std::nullopt si no existe.
     */
    static std::optional<fs::path> find_executable(const std::string &cmd) {
        if (cmd.empty()) return std::nullopt;

        // Si parece una ruta (contiene separador), verifique directamente.
        if (contains_dir_separator(cmd)) {
            fs::path p = normalize_path_safe(cmd);
#ifdef _WIN32
            // En Windows, si el usuario paso una ruta sin extension, pruebe con PATHEXT.
            if (path_exists_and_executable(p)) return p;
            if (!p.has_extension()) {
                const char *pathext_c = std::getenv("PATHEXT");
                std::string pathext = pathext_c ? pathext_c : ".COM;.EXE;.BAT;.CMD";
                std::transform(pathext.begin(), pathext.end(), pathext.begin(), ::toupper);
                auto exts = split_path_env(pathext);
                for (auto &e: exts) {
                    fs::path cand = p;
                    cand += e; // append extension
                    if (path_exists_and_executable(cand)) return cand;
                }
            }
            return std::nullopt;
#else
            return path_exists_and_executable(p) ? std::optional<fs::path>(p) : std::nullopt;
#endif
        }

        // No es una ruta: buscar en PATH
        const char *path_c = std::getenv("PATH");
        if (!path_c) return std::nullopt;
        std::string path_env(path_c);
        auto dirs = split_path_env(path_env);
#ifdef _WIN32
        // Windows: Considera PATHEXT
        const char *pathext_c = std::getenv("PATHEXT");
        std::string pathext = pathext_c ? pathext_c : ".COM;.EXE;.BAT;.CMD";
        std::transform(pathext.begin(), pathext.end(), pathext.begin(), ::toupper);
        auto exts = split_path_env(pathext);
        for (auto &d: dirs) {
            if (d.empty()) continue;
            fs::path base = fs::path(d) / cmd;
            // if cmd already has extension, check directly
            if (base.has_extension()) {
                if (path_exists_and_executable(base)) return normalize_path_safe(base);
            } else {
                for (auto &e: exts) {
                    fs::path cand = base;
                    cand += e;
                    if (path_exists_and_executable(cand)) return normalize_path_safe(cand);
                }
            }
        }
#else
        for (auto &d: dirs) {
            if (d.empty()) continue;
            fs::path cand = fs::path(d) / cmd;
            if (path_exists_and_executable(cand)) return normalize_path_safe(cand);
        }
#endif
        return std::nullopt;
    }

    /**
     * @brief Devuelve la ruta absoluta/canonical de una ruta existente.
     *
     * Si la ruta existe, intenta devolver su forma canonica (resolviendo enlaces
     * simbolicos y eliminando `.`/`..`) usando `fs::weakly_canonical` o `fs::canonical`.
     * Si la ruta no existe o no puede resolverse, devuelve std::nullopt.
     *
     * @param p Ruta (absoluta o relativa).
     * @return std::optional<fs::path> con la ruta absoluta/canonical si existe; std::nullopt en caso contrario.
     *
     * @note Esta funcion no lanza excepciones en el flujo normal: en caso de error
     *       devuelve std::nullopt. Entre la comprobacion y el uso real del fichero
     *       puede producirse una condicion TOCTOU; siempre maneja errores al abrir/usar.
     *
     * @code{.cpp}
     * // Ejemplo 1: archivo relativo existente
     * auto maybe_abs = vfs::get_existing_absolute_path("data/config.json");
     * if (maybe_abs) {
     *     std::cout << "Ruta absoluta: " << maybe_abs->string() << "\n";
     * } else {
     *     std::cout << "No existe o no se pudo resolver\n";
     * }
     *
     * // Ejemplo 2: ruta absoluta
     * auto maybe_abs2 = vfs::get_existing_absolute_path("/etc/hosts");
     * if (maybe_abs2) {
     *     // puede devolver la forma canonica (resolviendo symlinks)
     *     std::cout << "Canonical: " << maybe_abs2->string() << "\n";
     * }
     * @endcode
     */
    static std::optional<fs::path> get_existing_absolute_path(const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) return std::nullopt;

        try {
            fs::path can = fs::weakly_canonical(p);
            return can;
        } catch (const fs::filesystem_error &) {
            try {
                fs::path can2 = fs::canonical(p);
                return can2;
            } catch (const fs::filesystem_error &) {
                try {
                    fs::path abs = fs::absolute(p);
                    return abs.lexically_normal();
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
    }

    /**
     * @brief Comprueba si la ruta existe y es un directorio.
     *
     * @param p Ruta a comprobar.
     * @return true si existe y es un directorio; false en caso contrario o si ocurre un error.
     *
     * @code{.cpp}
     * // Ejemplo: comprobar directorio
     * if (vfs::is_directory("/var/log")) {
     *     std::cout << "/var/log es un directorio\n";
     * } else {
     *     std::cout << "/var/log no es un directorio o no existe\n";
     * }
     * @endcode
     */
    static bool is_directory(const fs::path &p) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) return false;
        return fs::is_directory(p, ec) && !ec;
    }

    /**
     * @brief Version que acepta std::string (normaliza y comprueba).
     *
     * Normaliza la cadena (relativa/absoluta) y devuelve la ruta absoluta/canonical
     * si el fichero o directorio existe.
     *
     * @param s Ruta en forma de cadena (absoluta o relativa).
     * @return std::optional<fs::path> con la ruta absoluta si existe; std::nullopt si no existe.
     *
     * @code{.cpp}
     * // Ejemplo: uso con std::string
     * std::string user_path = "./logs/app.log";
     * auto abs = vfs::get_existing_absolute_path_str(user_path);
     * if (abs) {
     *     std::cout << "Archivo existe en: " << abs->string() << "\n";
     * } else {
     *     std::cout << "Archivo no encontrado: " << user_path << "\n";
     * }
     * @endcode
     */
    static std::optional<fs::path> get_existing_absolute_path_str(const std::string &s) {
        return get_existing_absolute_path(normalize_path_safe(fs::path(s)));
    }

    /**
     * @brief Version string para is_directory (normaliza y comprueba).
     *
     * @param s Ruta en forma de cadena.
     * @return true si la ruta existe y es directorio.
     *
     * @code{.cpp}
     * // Ejemplo: comprobar si una entrada del usuario es directorio
     * std::string candidate = "build";
     * if (vfs::is_directory_str(candidate)) {
     *     std::cout << candidate << " es un directorio\n";
     * } else {
     *     std::cout << candidate << " no es un directorio o no existe\n";
     * }
     * @endcode
     */
    static bool is_directory_str(const std::string &s) {
        return fs::is_directory(normalize_path_safe(fs::path(s)));
    }

    /**
     * Obtener la ruta del ejecutable actual con el nombre del ejecutable includo
     * @return
     */
    static std::string get_executable_path() {
#ifdef _WIN32
        char buffer[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
        return std::string(buffer, len);
#elif __APPLE__
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        _NSGetExecutablePath(buffer, &size);
        return std::string(buffer);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        /* BSD no tiene procfs montado por defecto: la ruta del ejecutable
         * se pregunta al kernel con sysctl(KERN_PROC_PATHNAME). */
        char buffer[4096];
        size_t len = sizeof(buffer) - 1;
        int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
        if (sysctl(mib, 4, buffer, &len, nullptr, 0) == 0) {
            buffer[len] = '\0';
            return std::string(buffer);
        }
        return std::string();
#else
        char buffer[4096];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
        if (len < 0) return std::string();
        buffer[len] = '\0';
        return std::string(buffer);
#endif
    }

    /**
     * obtener nombre del ejecutable unicamente
     * @return nombre del ejecutable
     */
    static std::string get_executable_name() {
        std::string path = get_executable_path();
        return std::filesystem::path(path).filename().string();
    }
} // namespace vfs

#endif //FS_UTILS_H
