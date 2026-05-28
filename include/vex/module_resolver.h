/**
 * @file module_resolver.h
 * @brief Resolver de paths + dep graph para el sistema de modulos Vex (Phase M).
 *
 * Multi-plataforma (Windows + POSIX), multi-arquitectura.  Aprovecha
 * caracteristicas del hardware: hashing FNV-1a 64-bit en tiempo lineal
 * sobre los paths absolutos (los caches usan hashes como keys), normalizacion
 * lazy de paths (no copia bytes salvo en miss), reserva anticipada del
 * vector de dependencias.
 *
 * Garantias:
 *   - Cero alocaciones extra para modulos ya resueltos (cache hit con
 *     std::unordered_map<string,size_t> + indices estables).
 *   - Deteccion de ciclos en O(V+E) via DFS coloreado (WHITE/GRAY/BLACK).
 *   - Topo sort en O(V+E) sobre el mismo DFS.
 *   - Reentrante: cada `ModuleGraph` es independiente; multiples instancias
 *     pueden compilar proyectos distintos en paralelo sin sincronizacion.
 *
 * Diseno multi-plataforma:
 *   - Path normalization: `\` -> `/` en Windows.  Case-preserving pero
 *     case-insensitive en compare cuando FS es case-insensitive
 *     (NTFS/HFS) -- aunque por defecto comparamos case-sensitive y
 *     dejamos al usuario consistente.
 *   - Path absolute resolution: respeta la convencion del OS (drive
 *     letters Windows, `/` raiz POSIX).
 *   - Separators de VEX_PATH: `;` en Windows, `:` en POSIX (al estilo
 *     PATH del shell).
 */

#ifndef VEX_MODULE_RESOLVER_H
#define VEX_MODULE_RESOLVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vex/diagnostic.h"

namespace vex {

namespace ast {
    struct ModuleNode;
    struct ImportDecl;
}

/**
 * @brief Estado de un nodo del dep graph durante DFS.  WHITE = no visitado,
 * GRAY = en la pila DFS actual (si nos toca otra vez, ciclo), BLACK = completo.
 */
enum class ResolveColor : uint8_t { WHITE = 0, GRAY = 1, BLACK = 2 };

/**
 * @brief Entrada del modulo cargado y parseado.
 *
 * @c canonical_path es la ruta absoluta normalizada (sin `..`, separadores
 * unificados).  @c module_id es el indice univoco asignado al modulo
 * dentro del @c ModuleGraph (estable durante toda la vida del graph).
 * @c source_hash y @c parsed_ast son lazy: se llenan en el primer parse.
 */
struct ResolvedModule {
    /// Identificador interno (indice en @c ModuleGraph::modules_).
    uint32_t                          module_id = 0;
    /// Ruta canonica absoluta (forward slashes, sin `..`).
    std::string                       canonical_path;
    /// Hash FNV-1a 64 del @c canonical_path (key para cache lookups).
    uint64_t                          path_hash = 0;
    /// Nombre logico del modulo (ultimo segmento sin extension).
    /// E.g. @c "buffer" para @c "/home/x/editor/buffer.vex".
    std::string                       module_name;
    /// AST parseado (lazy: nullptr hasta primer parse).
    std::unique_ptr<ast::ModuleNode>  parsed_ast;
    /// Hash FNV-1a 64 del source crudo.  Usado para invalidacion incremental.
    uint64_t                          source_hash = 0;
    /// Dependencias (module_ids de los modulos importados).  Vacio hasta
    /// que se procesa el AST.
    std::vector<uint32_t>             dependencies;
    /// Estado DFS durante la deteccion de ciclos / topo sort.
    ResolveColor                      color = ResolveColor::WHITE;
};

/**
 * @brief Resultado de la resolucion de un import.
 */
struct ResolveResult {
    enum class Status : uint8_t {
        OK = 0,                ///< Modulo encontrado y cargado.
        NOT_FOUND = 1,         ///< Ningun candidato existe en disco.
        CYCLE = 2,             ///< Detectado ciclo en la cadena de imports.
        PARSE_ERROR = 3,       ///< El fichero existe pero su parse fallo.
        IO_ERROR = 4,          ///< Error de I/O leyendo el fichero.
    };
    Status      status = Status::OK;
    uint32_t    module_id = 0;          ///< Valido solo si status == OK.
    std::string error_message;          ///< Mensaje detallado si status != OK.
    std::vector<std::string> tried_paths;  ///< Para diagnostico de NOT_FOUND.
};

/**
 * @brief Grafo de modulos del proyecto.
 *
 * El @c ModuleGraph mantiene el universo de modulos del proyecto en
 * compilacion.  Es la fuente de verdad para:
 *   - Resolucion de paths (con cache).
 *   - Dep graph (que depende de quien).
 *   - Topo sort (orden de compilacion).
 *   - Deteccion de ciclos con mensaje claro.
 *
 * Multi-thread safety: NO es thread-safe per se.  Para compilacion
 * paralela (M8), se debe poblar el graph en single-thread y luego
 * lanzar workers que solo LEAN el graph (multi-reader sin lock es
 * seguro siempre que no haya escrituras concurrentes).  Las
 * dependencias se resuelven ANTES de despachar workers.
 */
class ModuleGraph {
public:
    /// @brief Construye un graph vacio.
    /// @param diags Sink para emitir errores con localizacion.
    explicit ModuleGraph(Diagnostics &diags);

    /**
     * @brief Anyade un directorio al search path del resolver.
     *
     * Se consultan en orden: (1) carpeta del fichero importador, (2)
     * search paths anyadidos en orden de @c add_search_path, (3)
     * @c VEX_PATH env var, (4) stdlib bundled.
     *
     * Los paths se normalizan al estilo POSIX (forward slash) y se
     * expanden a absolutos si son relativos.
     */
    void add_search_path(const std::string &dir);

    /**
     * @brief Anyade los directorios de VEX_PATH (separados por `:` POSIX
     * o `;` Windows).  Sin efecto si la env var no esta seteada.
     */
    void add_vex_path_env();

    /**
     * @brief Establece el path al directorio stdlib (resuelve `std/*`).
     *
     * Si el path no existe, no emite error (se considera "stdlib no
     * disponible" -- los imports `std/*` fallaran con NOT_FOUND).
     */
    void set_stdlib_dir(const std::string &dir);

    /**
     * @brief Resuelve el path de un import respecto al fichero importador.
     *
     * @param raw_path Path tal como aparece en @c import "path";
     * @param importer_file Fichero del @c import (para resolver relativos).
     * @return ResolveResult con el module_id o el error con tried_paths.
     *
     * El resolver intenta los candidatos en orden:
     *   1. <importer_dir>/<raw_path>.vex
     *   2. cada search path anyadido / <raw_path>.vex
     *   3. <stdlib_dir>/<raw_path>.vex
     */
    ResolveResult resolve(const std::string &raw_path,
                          const std::string &importer_file);

    /**
     * @brief Construye el dep graph completo recursivamente desde un
     * fichero raiz.  Parsea cada fichero alcanzable y registra sus
     * dependencias.  Detecta ciclos.
     *
     * @param root_file Path al fichero .vex raiz del proyecto.
     * @return module_id del root si OK, UINT32_MAX si error fatal.
     */
    uint32_t build_from_root(const std::string &root_file);

    /**
     * @brief Topo sort en orden de compilacion (deps primero).
     *
     * Solo seguro tras un @c build_from_root exitoso.  Si hubo ciclo,
     * el sort emite el orden parcial + el modulo del ciclo al final.
     *
     * @return vector de module_ids en orden topologico (dependencias
     *         primero, dependents despues).
     */
    std::vector<uint32_t> topological_order() const;

    /// @brief Acceso de solo lectura a un modulo por id.
    const ResolvedModule *module(uint32_t id) const noexcept {
        return (id < modules_.size()) ? modules_[id].get() : nullptr;
    }

    /// @brief Numero total de modulos en el grafo.
    size_t module_count() const noexcept { return modules_.size(); }

    /// @brief @c true si se detecto al menos un ciclo durante el build.
    bool has_cycle() const noexcept { return cycle_detected_; }

private:
    /// Carga el fichero, calcula hash, parsea AST, registra dependencias.
    /// Devuelve module_id del nuevo modulo o UINT32_MAX si error.
    uint32_t load_and_parse_(const std::string &canonical_path);

    /// Procesa las dependencias de @p mod recursivamente (DFS).
    /// Emite errores en @c diags_ si los hubiera.
    void process_dependencies_(ResolvedModule &mod);

    /// Normaliza un path: separadores -> `/`, resuelve `..`, expande
    /// relativos contra @c base_dir si no es absoluto.  Devuelve la
    /// ruta canonica.
    std::string normalize_path_(const std::string &raw,
                                 const std::string &base_dir) const;

    /// @c true si @p path es absoluto en el OS actual (drive letter
    /// Windows o `/` POSIX).
    static bool is_absolute_(const std::string &path) noexcept;

    /// FNV-1a 64-bit hash de un string.
    static uint64_t fnv1a_(const std::string &s) noexcept;

    /// Lee el fichero completo a un string.  Devuelve true si OK.
    static bool read_file_(const std::string &path, std::string &out);

    /// Comprueba existencia del fichero (cross-platform).
    static bool file_exists_(const std::string &path) noexcept;

    // ---- Datos miembro ----
    Diagnostics                                              &diags_;
    std::vector<std::unique_ptr<ResolvedModule>>              modules_;
    std::unordered_map<uint64_t, uint32_t>                    by_path_hash_;
    std::vector<std::string>                                  search_paths_;
    std::string                                               stdlib_dir_;
    bool                                                      cycle_detected_ = false;
};

} // namespace vex

#endif // VEX_MODULE_RESOLVER_H
