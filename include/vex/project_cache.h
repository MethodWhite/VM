/**
 * @file project_cache.h
 * @brief Phase M5.B - cache del @c .velb final del proyecto.
 *
 * Cache a nivel de bytecode-final: si nada cambio en el root + deps
 * recursivos + opciones de compile, el siguiente @c vm @c --vex es un
 * cache hit instantaneo (copia del @c .velb cacheado al output sin
 * invocar el frontend).
 *
 * Layout del cache file (binary, little-endian):
 * @verbatim
 * +0   u32  magic = 'VPCK' (0x4B435056)
 * +4   u16  format_version (=1)
 * +6   u16  _reserved
 * +8   u32  opts_hash (FNV-1a sobre las opciones de compile que afectan
 *           la salida -- opt_level, emit_debug, vex_base, etc.)
 * +12  u32  dep_count
 * +16  DepEntry[dep_count]:
 *           u32  path_len
 *           u8   path[path_len]
 *           u64  source_hash (FNV-1a 64 del source crudo)
 * +    u32  velb_size
 * +    u8   velb_bytes[velb_size]
 * @endverbatim
 *
 * El cache file vive en `<cache_dir>/<root_hash>.vpc`.  El @c root_hash
 * es FNV-1a 64 del path canonico del root.
 */

#ifndef VEX_PROJECT_CACHE_H
#define VEX_PROJECT_CACHE_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

/// Phase M5.B: opciones que afectan el output del compile.  Cualquier
/// cambio en estos campos invalida el cache (se incluye en opts_hash).
struct ProjectCacheKey {
    int          opt_level     = 1;
    bool         emit_debug    = false;
    uint64_t     vex_base      = 0;
    std::string  instrument_mode;     ///< "none", "trace", "coverage", etc.
    std::string  port_target;         ///< "" si no es port; "c", "java", etc.
};

/// Phase M5.B: entrada por modulo en el cache file.  Tras un compile
/// exitoso, el caller persiste @c path + @c source_hash de cada modulo
/// participante (root + deps recursivos).
struct ProjectCacheDep {
    std::string path;          ///< Path canonico (absoluto + normalizado).
    uint64_t    source_hash;   ///< FNV-1a 64 del source crudo.
};

/// Phase M5.B: API publica.
///
/// @brief Computa el path donde se cachearia el .velb del proyecto.
/// @param root_path  Path canonico del root.
/// @param cache_dir  Directorio del cache global (puede ser una funcion
///                   centralizada en @c $VEX_HOME o @c ./.vex_cache ).
std::string project_cache_path(const std::string &root_path,
                                const std::string &cache_dir);

/// @brief Computa el directorio default del cache de proyectos.
/// Por defecto: @c $VEX_HOME/cache/projects o @c ./.vex_cache/projects .
std::string default_project_cache_dir();

/// @brief Lee un cache file y devuelve sus contenidos parseados.
/// @return @c true si el archivo existe, magic + version validos.
///         Los campos out_* se llenan en caso OK.  En miss/error,
///         devuelve @c false y los out_* quedan vacios.
bool project_cache_load(const std::string &cache_path,
                         uint32_t &out_opts_hash,
                         std::vector<ProjectCacheDep> &out_deps,
                         std::vector<uint8_t> &out_velb);

/// @brief Escribe un cache file con los contenidos dados.  Usa atomic
/// rename (igual que @c write_file_atomic_ del compiler_project).
/// @return @c true si exito.
bool project_cache_save(const std::string &cache_path,
                         uint32_t opts_hash,
                         const std::vector<ProjectCacheDep> &deps,
                         const std::vector<uint8_t> &velb);

/// @brief Computa el @c opts_hash sobre la @c ProjectCacheKey .
/// FNV-1a 32 sobre los campos concatenados.
uint32_t project_cache_opts_hash(const ProjectCacheKey &key);

/// @brief Verifica si el cache esta vigente: dado el cache file
/// cargado, leer cada @c ProjectCacheDep::path del disco, calcular su
/// FNV-1a actual y comparar.  Si todos coinciden, cache HIT.
/// @return @c true si los hashes actuales coinciden con los cacheados.
bool project_cache_validate(const std::vector<ProjectCacheDep> &cached_deps);

/// @brief FNV-1a 64 helper (publico para uso en @c compile_vex_project).
uint64_t fnv1a64_bytes(const uint8_t *data, size_t size) noexcept;

} // namespace vex

#endif // VEX_PROJECT_CACHE_H
