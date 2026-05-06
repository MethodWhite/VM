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
 * @file package.h
 * @brief Sistema de paquetes de VestaVM: manifiesto, resolucion y carga dinamica.
 *
 * Un paquete de VestaVM es un directorio con un archivo opcional "package.vel"
 * que declara metadatos y dependencias.  Sin ese archivo, el directorio se
 * trata como paquete anonimo con un unico modulo.
 *
 * Formato de package.vel (anotacion declarativa):
 * @code
 *   @Package {
 *       @Name("com.myapp.core")
 *       @Version("1.2.0")
 *       @Author("nombre opcional")
 *       @Entry("main")
 *       @Depends {
 *           @Dep("stdlib/native/collections" ">=1.0")
 *           @Dep("com.myapp.util"            "1.0.0")
 *       }
 *   }
 * @endcode
 *
 * Todos los campos son opcionales excepto @Name cuando se quieren exports
 * publicos desde otros paquetes.
 *
 * Resolucion de dependencias:
 *   1. Se lee package.vel del directorio raiz del proyecto.
 *   2. Para cada @Dep, se busca en VESTA_PKG_PATH (separado por ':' en Linux,
 *      ';' en Windows) y luego en el directorio actual.
 *   3. Las dependencias se cargan en orden topologico (DFS con deteccion de
 *      ciclos).  Un ciclo entre paquetes es error fatal.
 *   4. Cada paquete produce uno o varios .velb que se pasan al linker antes
 *      de ejecutar.
 *
 * Carga dinamica vs. compilacion estatica:
 *   - PackageLoader::load_static()  -- resuelve y vincula en compile-time;
 *                                      produce un .velb unificado.
 *   - PackageLoader::load_dynamic() -- carga .velb a la VM en runtime;
 *                                      permite hot-reload de modulos.
 *
 * Compatibilidad de versiones (semantica semver simplificada):
 *   - "1.2.3"   exactamente esa version
 *   - ">=1.2"   mayor o igual
 *   - "~1.2"    compatible: >= 1.2.0 y < 1.3.0
 *   - "*"       cualquier version
 */

#ifndef PACKAGE_H
#define PACKAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constantes del sistema de paquetes
 * ========================================================================= */

/** @brief Nombre del archivo de manifiesto de paquete. */
#define PKG_MANIFEST_FILE  "package.vel"

/** @brief Longitud maxima de un nombre de paquete. */
#define PKG_NAME_MAX       256u

/** @brief Longitud maxima de una cadena de version. */
#define PKG_VERSION_MAX    64u

/** @brief Numero maximo de dependencias directas por paquete. */
#define PKG_MAX_DEPS       64u

/** @brief Longitud maxima de una ruta de directorio. */
#define PKG_PATH_MAX       512u

/** @brief Variable de entorno con la lista de directorios de busqueda. */
#define PKG_PATH_ENV       "VESTA_PKG_PATH"

/* =========================================================================
 * Tipos de version
 * ========================================================================= */

/**
 * @brief Operador de restriccion de version.
 */
typedef enum PkgVersionOp {
    PKG_VER_EXACT   = 0, /**< "1.2.3"  -- version exacta */
    PKG_VER_GTE     = 1, /**< ">=1.2"  -- mayor o igual */
    PKG_VER_COMPAT  = 2, /**< "~1.2"   -- compatible (minor fijo) */
    PKG_VER_ANY     = 3  /**< "*"      -- cualquier version */
} PkgVersionOp;

/**
 * @brief Version semantica simplificada: major.minor.patch.
 */
typedef struct PkgVersion {
    uint32_t     major;   /**< componente mayor */
    uint32_t     minor;   /**< componente menor */
    uint32_t     patch;   /**< parche */
    PkgVersionOp op;      /**< operador de restriccion */
    char         raw[PKG_VERSION_MAX]; /**< cadena original (ej: ">=1.2.0") */
} PkgVersion;

/* =========================================================================
 * Dependencia de paquete
 * ========================================================================= */

/**
 * @brief Declaracion de una dependencia de paquete.
 *
 * Corresponde a una linea @Dep("nombre" "version") en package.vel.
 */
typedef struct PkgDep {
    char       name[PKG_NAME_MAX];    /**< nombre del paquete requerido */
    PkgVersion version;               /**< restriccion de version */
} PkgDep;

/* =========================================================================
 * Manifiesto de paquete
 * ========================================================================= */

/**
 * @brief Manifiesto completo de un paquete de VestaVM.
 *
 * Parseado desde package.vel.  Si no existe package.vel, se genera un
 * manifiesto anonimo con name="" y dep_count=0.
 */
typedef struct PkgManifest {
    char       name[PKG_NAME_MAX];    /**< nombre calificado (p.ej. "com.myapp.core") */
    char       version_str[PKG_VERSION_MAX]; /**< version como cadena */
    PkgVersion version;               /**< version parseada */
    char       author[PKG_NAME_MAX];  /**< autor del paquete (opcional) */
    char       entry[PKG_NAME_MAX];   /**< simbolo de entrada principal (opcional) */
    char       source_dir[PKG_PATH_MAX]; /**< directorio raiz del paquete */
    PkgDep     deps[PKG_MAX_DEPS];    /**< dependencias declaradas */
    size_t     dep_count;             /**< numero de dependencias */
    bool       anonymous;             /**< true si no hay package.vel */
} PkgManifest;

/* =========================================================================
 * Estado de un paquete resuelto
 * ========================================================================= */

/**
 * @brief Estado del ciclo de vida de un paquete durante la resolucion.
 */
typedef enum PkgState {
    PKG_UNVISITED = 0, /**< no procesado aun */
    PKG_IN_STACK  = 1, /**< en la pila DFS (detectar ciclos) */
    PKG_RESOLVED  = 2  /**< dependencias satisfechas */
} PkgState;

/**
 * @brief Nodo en el grafo de dependencias.
 */
typedef struct PkgNode {
    PkgManifest manifest;          /**< datos del paquete */
    PkgState    state;             /**< estado DFS */
    char        velb_path[PKG_PATH_MAX]; /**< ruta al .velb compilado (si existe) */
    bool        is_native;         /**< true si el paquete es un plugin .dll/.so */
} PkgNode;

/* =========================================================================
 * Error del sistema de paquetes
 * ========================================================================= */

/**
 * @brief Codigo de error del sistema de paquetes.
 */
typedef enum PkgError {
    PKG_OK           = 0, /**< sin error */
    PKG_ERR_NOTFOUND = 1, /**< paquete no encontrado en VESTA_PKG_PATH */
    PKG_ERR_CYCLE    = 2, /**< ciclo de dependencias detectado */
    PKG_ERR_VERSION  = 3, /**< version incompatible */
    PKG_ERR_PARSE    = 4, /**< error al parsear package.vel */
    PKG_ERR_IO       = 5, /**< error de E/S al leer/escribir archivos */
    PKG_ERR_NOMEM    = 6  /**< fallo de asignacion de memoria */
} PkgError;

/* =========================================================================
 * Resultado de carga
 * ========================================================================= */

/**
 * @brief Resultado de la carga de paquetes.
 *
 * Contiene la lista de .velb en orden topologico de carga y los plugins
 * nativos que deben cargarse antes de ejecutar el bytecode.
 */
typedef struct PkgLoadResult {
    char    **velb_paths;    /**< rutas a los .velb en orden topologico */
    size_t    velb_count;    /**< numero de .velb */
    char    **native_paths;  /**< rutas a los plugins nativos (.dll/.so) */
    size_t    native_count;  /**< numero de plugins nativos */
    PkgError  error;         /**< PKG_OK si todo fue bien */
    char      error_msg[256];/**< mensaje de error legible (si error != PKG_OK) */
} PkgLoadResult;

/* =========================================================================
 * API del PackageLoader
 * ========================================================================= */

#ifdef __cplusplus
} /* extern "C" */

#include <vector>
#include <string>
#include <unordered_map>

namespace loader {

    /**
     * @brief Cargador y resolvedor de paquetes de VestaVM.
     *
     * Uso tipico:
     * @code
     *   PackageLoader pl;
     *   pl.add_search_path("/opt/vesta/packages");
     *
     *   PkgManifest root;
     *   pl.parse_manifest("myproject/package.vel", root);
     *
     *   PkgLoadResult result = pl.resolve(root);
     *   if (result.error != PKG_OK) {
     *       // manejar error
     *   }
     *   // pasar result.velb_paths al linker
     * @endcode
     */
    class PackageLoader {
    public:
        /**
         * @brief Constructor.  Lee VESTA_PKG_PATH del entorno automaticamente.
         */
        PackageLoader();

        /**
         * @brief Anade un directorio de busqueda de paquetes.
         * @param path Ruta absoluta del directorio.
         */
        void add_search_path(const std::string &path);

        /**
         * @brief Parsea un archivo package.vel y rellena el manifiesto.
         *
         * Si el archivo no existe, genera un manifiesto anonimo.
         * El formato es un subconjunto de la sintaxis de anotaciones .vel.
         *
         * @param manifest_path Ruta al package.vel.
         * @param out           Manifiesto de salida.
         * @return PKG_OK o codigo de error.
         */
        PkgError parse_manifest(const std::string &manifest_path,
                                 PkgManifest &out);

        /**
         * @brief Resuelve el arbol de dependencias en orden topologico.
         *
         * Algoritmo DFS con deteccion de ciclos.  Para cada dependencia,
         * busca el paquete en los directorios de busqueda y carga su
         * manifiesto recursivamente.
         *
         * @param root Manifiesto del paquete raiz.
         * @return PkgLoadResult con las rutas en orden de carga.
         */
        PkgLoadResult resolve(const PkgManifest &root);

        /**
         * @brief Comprueba si una version satisface una restriccion.
         *
         * @param have    Version disponible del paquete encontrado.
         * @param require Restriccion de version solicitada.
         * @return true si es compatible.
         */
        static bool version_satisfies(const PkgVersion &have,
                                      const PkgVersion &require);

        /**
         * @brief Parsea una cadena de version como "1.2.3" o ">=1.0".
         *
         * @param str Cadena de version.
         * @param out Version parseada.
         * @return true si el parseo fue exitoso.
         */
        static bool parse_version(const char *str, PkgVersion &out);

    private:
        /**
         * @brief Visita un nodo en el DFS de resolucion.
         *
         * @param name     Nombre del paquete a visitar.
         * @param result   Resultado acumulado.
         * @param visited  Mapa de nodos ya visitados (nombre -> estado).
         * @return PKG_OK o error.
         */
        PkgError visit(const std::string &name,
                       PkgLoadResult &result,
                       std::unordered_map<std::string, PkgNode> &visited);

        /**
         * @brief Busca el directorio de un paquete en los search paths.
         *
         * @param name     Nombre del paquete (p.ej. "com.myapp.util").
         * @param out_dir  Ruta encontrada.
         * @return true si se encontro.
         */
        bool find_package_dir(const std::string &name,
                              std::string &out_dir) const;

        std::vector<std::string> search_paths_; /**< directorios de busqueda */
    };

} // namespace loader

#endif /* __cplusplus */

#endif /* PACKAGE_H */
