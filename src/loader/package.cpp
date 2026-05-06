/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file package.cpp
 * @brief Implementacion del sistema de paquetes de VestaVM.
 *
 * Parseo de package.vel, resolucion de dependencias en orden topologico
 * (DFS con deteccion de ciclos), busqueda en VESTA_PKG_PATH y validacion
 * de versiones semanticas simplificadas.
 */

#include "loader/package.h"

#include <cstring>
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>

namespace fs = std::filesystem;

namespace loader {

    /* =====================================================================
     * parse_version
     * ===================================================================== */

    /**
     * @brief Parsea "major.minor.patch" o ">=major.minor" en PkgVersion.
     *
     * Formatos soportados:
     *   "1.2.3"   -> EXACT  1.2.3
     *   ">=1.2"   -> GTE    1.2.0
     *   "~1.2"    -> COMPAT 1.2.0
     *   "*"       -> ANY    0.0.0
     *
     * @param str Cadena de version terminada en '\0'.
     * @param out Version parseada.
     * @return true si el parseo fue exitoso.
     */
    bool PackageLoader::parse_version(const char *str, PkgVersion &out) {
        if (!str || *str == '\0') return false;

        // guardar cadena original
        std::strncpy(out.raw, str, PKG_VERSION_MAX - 1);
        out.raw[PKG_VERSION_MAX - 1] = '\0';

        out.major = 0;
        out.minor = 0;
        out.patch = 0;

        if (str[0] == '*') {
            out.op = PKG_VER_ANY;
            return true;
        }

        const char *p = str;
        if (p[0] == '>' && p[1] == '=') {
            out.op = PKG_VER_GTE;
            p += 2;
        } else if (p[0] == '~') {
            out.op = PKG_VER_COMPAT;
            p += 1;
        } else {
            out.op = PKG_VER_EXACT;
        }

        // parsear major.minor.patch con sscanf
        int parsed = sscanf(p, "%u.%u.%u", &out.major, &out.minor, &out.patch);
        return parsed >= 1; // al menos major
    }

    /* =====================================================================
     * version_satisfies
     * ===================================================================== */

    /**
     * @brief Comprueba si la version disponible satisface la restriccion.
     *
     * @param have    Version del paquete encontrado.
     * @param require Restriccion declarada por el dependiente.
     * @return true si es compatible.
     */
    bool PackageLoader::version_satisfies(const PkgVersion &have,
                                          const PkgVersion &require) {
        switch (require.op) {
            case PKG_VER_ANY:
                return true; // cualquier version es valida

            case PKG_VER_EXACT:
                // major, minor y patch deben coincidir exactamente
                return have.major == require.major &&
                       have.minor == require.minor &&
                       have.patch == require.patch;

            case PKG_VER_GTE: {
                // have >= require lexicograficamente (major, minor, patch)
                if (have.major != require.major) return have.major > require.major;
                if (have.minor != require.minor) return have.minor > require.minor;
                return have.patch >= require.patch;
            }

            case PKG_VER_COMPAT: {
                // mismo major y minor; patch libre
                return have.major == require.major &&
                       have.minor == require.minor;
            }
        }
        return false;
    }

    /* =====================================================================
     * PackageLoader: constructor
     * ===================================================================== */

    /**
     * @brief Constructor: lee VESTA_PKG_PATH del entorno.
     *
     * VESTA_PKG_PATH usa ':' como separador en Unix y ';' en Windows,
     * igual que PATH del sistema operativo.
     */
    PackageLoader::PackageLoader() {
        const char *env = std::getenv(PKG_PATH_ENV);
        if (!env) return;

#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string s(env);
        std::stringstream ss(s);
        std::string token;
        while (std::getline(ss, token, sep)) {
            if (!token.empty()) search_paths_.push_back(token);
        }
    }

    /**
     * @brief Anade un directorio de busqueda.
     */
    void PackageLoader::add_search_path(const std::string &path) {
        search_paths_.push_back(path);
    }

    /* =====================================================================
     * parse_manifest
     * ===================================================================== */

    /**
     * @brief Parsea un package.vel minimal y rellena PkgManifest.
     *
     * El parser es un extractor de tokens simple: no usa el lexer completo
     * de VestaVM para evitar dependencias circulares.  Extrae los campos
     * @Name, @Version, @Author, @Entry y @Dep mediante expresiones regulares
     * sobre el contenido del archivo.
     *
     * Si el archivo no existe o no se puede leer, genera un manifiesto
     * anonimo con anonymous=true.
     *
     * @param manifest_path Ruta al package.vel.
     * @param out           Manifiesto de salida.
     * @return PKG_OK o PKG_ERR_PARSE si el contenido es invalido.
     */
    PkgError PackageLoader::parse_manifest(const std::string &manifest_path,
                                            PkgManifest &out) {
        std::memset(&out, 0, sizeof(out));

        // guardar directorio raiz del paquete
        fs::path mp(manifest_path);
        std::string dir = mp.parent_path().string();
        std::strncpy(out.source_dir, dir.c_str(), PKG_PATH_MAX - 1);

        // si no existe el manifiesto, paquete anonimo
        std::ifstream f(manifest_path);
        if (!f.is_open()) {
            out.anonymous = true;
            return PKG_OK;
        }

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        // --- extraer @Name("...") ---
        // Nota: el delimitador 'rx' evita que )"  dentro del patron termine el raw string
        {
            std::regex re(R"rx(@Name\s*\(\s*"([^"]+)"\s*\))rx");
            std::smatch m;
            if (std::regex_search(content, m, re)) {
                std::strncpy(out.name, m[1].str().c_str(), PKG_NAME_MAX - 1);
            }
        }
        // --- extraer @Version("...") ---
        {
            std::regex re(R"rx(@Version\s*\(\s*"([^"]+)"\s*\))rx");
            std::smatch m;
            if (std::regex_search(content, m, re)) {
                std::strncpy(out.version_str, m[1].str().c_str(), PKG_VERSION_MAX - 1);
                parse_version(out.version_str, out.version);
            }
        }
        // --- extraer @Author("...") ---
        {
            std::regex re(R"rx(@Author\s*\(\s*"([^"]+)"\s*\))rx");
            std::smatch m;
            if (std::regex_search(content, m, re)) {
                std::strncpy(out.author, m[1].str().c_str(), PKG_NAME_MAX - 1);
            }
        }
        // --- extraer @Entry("...") ---
        {
            std::regex re(R"rx(@Entry\s*\(\s*"([^"]+)"\s*\))rx");
            std::smatch m;
            if (std::regex_search(content, m, re)) {
                std::strncpy(out.entry, m[1].str().c_str(), PKG_NAME_MAX - 1);
            }
        }
        // --- extraer @Dep("nombre" "version") (puede haber varios) ---
        {
            std::regex re(R"rx(@Dep\s*\(\s*"([^"]+)"\s+"([^"]+)"\s*\))rx");
            auto begin = std::sregex_iterator(content.begin(), content.end(), re);
            auto end   = std::sregex_iterator();
            for (auto it = begin; it != end && out.dep_count < PKG_MAX_DEPS; ++it) {
                std::smatch m = *it;
                PkgDep &dep = out.deps[out.dep_count++];
                std::strncpy(dep.name,    m[1].str().c_str(), PKG_NAME_MAX - 1);
                parse_version(m[2].str().c_str(), dep.version);
            }
        }

        out.anonymous = (out.name[0] == '\0');
        return PKG_OK;
    }

    /* =====================================================================
     * find_package_dir
     * ===================================================================== */

    /**
     * @brief Busca el directorio de un paquete en los search paths.
     *
     * Traduce el nombre "com.myapp.util" a la ruta "com/myapp/util" y
     * busca un directorio con ese nombre (o con el nombre sin traduccion)
     * en cada search path.  Tambien busca directamente como ruta relativa.
     *
     * @param name    Nombre del paquete.
     * @param out_dir Directorio encontrado.
     * @return true si se encontro.
     */
    bool PackageLoader::find_package_dir(const std::string &name,
                                          std::string &out_dir) const {
        // traducir dots a separadores de directorio
        std::string rel = name;
        std::replace(rel.begin(), rel.end(), '.', '/');

        auto try_dir = [&](const std::string &base) -> bool {
            // intentar base/com/myapp/util
            fs::path p1 = fs::path(base) / rel;
            if (fs::is_directory(p1)) { out_dir = p1.string(); return true; }
            // intentar base/com.myapp.util (nombre literal como subdirectorio)
            fs::path p2 = fs::path(base) / name;
            if (fs::is_directory(p2)) { out_dir = p2.string(); return true; }
            return false;
        };

        for (const auto &sp : search_paths_) {
            if (try_dir(sp)) return true;
        }
        // buscar en directorio actual como ultimo recurso
        if (try_dir(".")) return true;
        return false;
    }

    /* =====================================================================
     * visit (DFS)
     * ===================================================================== */

    /**
     * @brief DFS para resolver dependencias del paquete con nombre dado.
     *
     * Detecta ciclos con el estado PKG_IN_STACK.  Las rutas .velb de cada
     * paquete se anaden a result.velb_paths en orden postorder (las
     * dependencias antes que el dependiente).
     *
     * @param name     Nombre del paquete a resolver.
     * @param result   Resultado acumulado.
     * @param visited  Mapa de nodos visitados (nombre -> PkgNode).
     * @return PKG_OK o codigo de error.
     */
    PkgError PackageLoader::visit(
            const std::string &name,
            PkgLoadResult &result,
            std::unordered_map<std::string, PkgNode> &visited) {

        // ciclo detectado
        auto it = visited.find(name);
        if (it != visited.end()) {
            if (it->second.state == PKG_IN_STACK) {
                result.error = PKG_ERR_CYCLE;
                std::snprintf(result.error_msg, sizeof(result.error_msg),
                              "ciclo de dependencias detectado en '%s'",
                              name.c_str());
                return PKG_ERR_CYCLE;
            }
            if (it->second.state == PKG_RESOLVED) return PKG_OK; // ya procesado
        }

        // crear nodo y marcarlo en pila
        PkgNode node{};
        node.state = PKG_IN_STACK;
        visited[name] = node;
        PkgNode &cur = visited[name];

        // buscar directorio del paquete
        std::string pkg_dir;
        bool is_native = false;

        // detectar si es un plugin nativo (ruta contiene "native/")
        if (name.find("stdlib/native") != std::string::npos ||
            name.find("native/")       != std::string::npos) {
            is_native = true;
        }

        if (!is_native) {
            if (!find_package_dir(name, pkg_dir)) {
                result.error = PKG_ERR_NOTFOUND;
                std::snprintf(result.error_msg, sizeof(result.error_msg),
                              "paquete '%s' no encontrado en VESTA_PKG_PATH",
                              name.c_str());
                return PKG_ERR_NOTFOUND;
            }
        }

        cur.is_native = is_native;

        if (!is_native) {
            // parsear manifiesto del paquete encontrado
            std::string mf = pkg_dir + "/" PKG_MANIFEST_FILE;
            PkgError pe = parse_manifest(mf, cur.manifest);
            if (pe != PKG_OK) { result.error = pe; return pe; }

            std::strncpy(cur.manifest.source_dir, pkg_dir.c_str(), PKG_PATH_MAX - 1);

            // resolver cada dependencia recursivamente
            for (size_t i = 0; i < cur.manifest.dep_count; i++) {
                PkgError de = visit(cur.manifest.deps[i].name, result, visited);
                if (de != PKG_OK) return de;
            }

            // buscar el .velb precompilado del paquete
            fs::path velb_p = fs::path(pkg_dir) /
                              (std::string(cur.manifest.name) + ".velb");
            if (!fs::exists(velb_p)) {
                // intentar nombre del directorio + .velb
                velb_p = fs::path(pkg_dir) /
                         (fs::path(pkg_dir).filename().string() + ".velb");
            }
            if (fs::exists(velb_p)) {
                std::strncpy(cur.velb_path, velb_p.string().c_str(), PKG_PATH_MAX - 1);
                // anadir al resultado (postorder: dependencias antes que yo)
                result.velb_paths[result.velb_count] =
                    (char *)std::malloc(PKG_PATH_MAX);
                if (result.velb_paths[result.velb_count]) {
                    std::strncpy(result.velb_paths[result.velb_count],
                                 cur.velb_path, PKG_PATH_MAX - 1);
                    result.velb_count++;
                }
            }
        } else {
            // plugin nativo: anadir a native_paths
            result.native_paths[result.native_count] =
                (char *)std::malloc(PKG_PATH_MAX);
            if (result.native_paths[result.native_count]) {
                std::strncpy(result.native_paths[result.native_count],
                             name.c_str(), PKG_PATH_MAX - 1);
                result.native_count++;
            }
        }

        cur.state = PKG_RESOLVED; // marcar como listo
        visited[name] = cur;
        return PKG_OK;
    }

    /* =====================================================================
     * resolve
     * ===================================================================== */

    /**
     * @brief Resuelve el arbol completo de dependencias de un paquete raiz.
     *
     * @param root Manifiesto del paquete raiz (parseado con parse_manifest).
     * @return PkgLoadResult con las rutas en orden de carga.
     */
    PkgLoadResult PackageLoader::resolve(const PkgManifest &root) {
        PkgLoadResult result{};
        result.error = PKG_OK;

        // reservar capacidad para las rutas (se amplia si es necesario)
        const size_t MAX_PKGS = 512;
        result.velb_paths   = (char **)std::calloc(MAX_PKGS, sizeof(char *));
        result.native_paths = (char **)std::calloc(MAX_PKGS, sizeof(char *));
        if (!result.velb_paths || !result.native_paths) {
            result.error = PKG_ERR_NOMEM;
            std::strncpy(result.error_msg, "sin memoria para resolver paquetes",
                         sizeof(result.error_msg) - 1);
            return result;
        }

        std::unordered_map<std::string, PkgNode> visited;
        visited.reserve(64);

        // resolver cada dependencia directa del raiz
        for (size_t i = 0; i < root.dep_count; i++) {
            PkgError pe = visit(root.deps[i].name, result, visited);
            if (pe != PKG_OK) return result;
        }

        return result;
    }

    /* =====================================================================
     * pkg_load_result_free (funcion libre para C)
     * ===================================================================== */

} // namespace loader

extern "C" {

    /**
     * @brief Libera la memoria de un PkgLoadResult.
     *
     * Llama a esta funcion cuando el resultado ya no sea necesario para
     * evitar fugas de memoria en las rutas asignadas por resolve().
     *
     * @param result Puntero al resultado a liberar.
     */
    void pkg_load_result_free(PkgLoadResult *result) {
        if (!result) return;
        for (size_t i = 0; i < result->velb_count; i++) {
            std::free(result->velb_paths[i]);
        }
        std::free(result->velb_paths);
        result->velb_paths  = NULL;
        result->velb_count  = 0;

        for (size_t i = 0; i < result->native_count; i++) {
            std::free(result->native_paths[i]);
        }
        std::free(result->native_paths);
        result->native_paths  = NULL;
        result->native_count  = 0;
    }

} // extern "C"
