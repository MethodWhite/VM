/**
 * @file project_cache.cpp
 * @brief Implementacion del cache de proyecto a nivel @c .velb (Phase M5.B).
 *
 * Cache file con magic VPCK + lista de (path, source_hash) de cada modulo
 * participante en el compile + el @c .velb final.  Lookup rapido:
 * leer la lista, recomputar FNV-1a de cada source, comparar con
 * cacheado.  Si todo matchea, cache hit; el caller copia el @c .velb
 * cacheado al output.
 */

#include "vex/project_cache.h"
#include "vex/vexi_format.h"     // para vexi_compiler_version_hash() (L.15)

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace vex {

namespace {

constexpr uint32_t VPC_MAGIC          = 0x4B435056;  // 'VPCK' LE
// v2 (M.L14+L15): anyade compiler_version_hash u64 tras opts_hash.
constexpr uint16_t VPC_FORMAT_VERSION = 2;

void write_u16_le(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void write_u32_le(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void write_u64_le(std::vector<uint8_t> &out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

bool read_u16_le(const uint8_t *data, size_t size, size_t &off, uint16_t &v) {
    if (off + 2 > size) return false;
    v = static_cast<uint16_t>(data[off])
      | (static_cast<uint16_t>(data[off+1]) << 8);
    off += 2;
    return true;
}
bool read_u32_le(const uint8_t *data, size_t size, size_t &off, uint32_t &v) {
    if (off + 4 > size) return false;
    v = static_cast<uint32_t>(data[off])
      | (static_cast<uint32_t>(data[off+1]) << 8)
      | (static_cast<uint32_t>(data[off+2]) << 16)
      | (static_cast<uint32_t>(data[off+3]) << 24);
    off += 4;
    return true;
}
bool read_u64_le(const uint8_t *data, size_t size, size_t &off, uint64_t &v) {
    if (off + 8 > size) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[off + i]) << (i * 8);
    }
    off += 8;
    return true;
}

bool read_file_bytes_internal(const std::string &path,
                                std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char *>(out.data()), sz);
    return f.good();
}

bool write_file_atomic_internal(const std::string &path,
                                  const std::vector<uint8_t> &bytes) {
    namespace fs = std::filesystem;
    static std::atomic<uint64_t> tmp_counter{0};
    try {
        fs::create_directories(fs::path(path).parent_path());
    } catch (...) { /* ignorar */ }
    std::ostringstream suffix;
    suffix << ".tmp."
#ifdef _WIN32
           << static_cast<uint64_t>(GetCurrentProcessId())
#else
           << static_cast<uint64_t>(getpid())
#endif
           << "." << tmp_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string tmp_path = path + suffix.str();
    {
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f.is_open()) return false;
        if (!bytes.empty()) {
            f.write(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }
        if (!f.good()) {
            f.close();
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        fs::copy_file(tmp_path, path,
                       fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_path, ec);
        return !ec;
    }
    return true;
}

} // namespace

uint64_t fnv1a64_bytes(const uint8_t *data, size_t size) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint32_t fnv1a32_str(const std::string &s) noexcept {
    uint32_t h = 0x811c9dc5U;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x01000193U;
    }
    return h;
}

std::string default_project_cache_dir() {
    namespace fs = std::filesystem;
    // Preferencia: VEX_HOME/cache/projects, fallback ./.vex_cache/projects.
    if (const char *vh = std::getenv("VEX_HOME")) {
        if (vh && vh[0]) {
            return (fs::path(vh) / "cache" / "projects").string();
        }
    }
    // Fallback al cwd actual.
    return (fs::current_path() / ".vex_cache" / "projects").string();
}

std::string project_cache_path(const std::string &root_path,
                                const std::string &cache_dir) {
    namespace fs = std::filesystem;
    // root_hash = FNV-1a 64 del path canonico.  Asi cada root tiene
    // su propio cache file estable.
    const uint64_t h = fnv1a64_bytes(
        reinterpret_cast<const uint8_t *>(root_path.data()),
        root_path.size());
    std::ostringstream fname;
    fname << std::hex << h << ".vpc";
    return (fs::path(cache_dir) / fname.str()).string();
}

uint32_t project_cache_opts_hash(const ProjectCacheKey &key) {
    // Concatenamos los campos en un string y hashing FNV-1a 32.  El
    // formato exacto del string no importa mientras sea estable.
    std::ostringstream os;
    os << "opt=" << key.opt_level
       << "|debug=" << (key.emit_debug ? 1 : 0)
       << "|base=0x" << std::hex << key.vex_base
       << "|instr=" << key.instrument_mode
       << "|port=" << key.port_target;
    return fnv1a32_str(os.str());
}

bool project_cache_load(const std::string &cache_path,
                         uint32_t &out_opts_hash,
                         std::vector<ProjectCacheDep> &out_deps,
                         std::vector<uint8_t> &out_velb) {
    out_opts_hash = 0;
    out_deps.clear();
    out_velb.clear();

    std::vector<uint8_t> buf;
    if (!read_file_bytes_internal(cache_path, buf)) return false;
    if (buf.size() < 24) return false;  // header v2: magic+ver+pad+opts+cvh+dep_count

    size_t off = 0;
    uint32_t magic = 0;
    uint16_t version = 0, reserved = 0;
    uint32_t dep_count = 0;
    if (!read_u32_le(buf.data(), buf.size(), off, magic))        return false;
    if (!read_u16_le(buf.data(), buf.size(), off, version))      return false;
    if (!read_u16_le(buf.data(), buf.size(), off, reserved))     return false;
    if (!read_u32_le(buf.data(), buf.size(), off, out_opts_hash))return false;
    // Phase M.L15: compiler_version_hash.  Si el binario del compilador
    // cambio (build distinto, version distinta), invalidar el cache.
    uint64_t cached_cvh = 0;
    if (!read_u64_le(buf.data(), buf.size(), off, cached_cvh))   return false;
    if (!read_u32_le(buf.data(), buf.size(), off, dep_count))    return false;
    if (magic != VPC_MAGIC)              return false;
    if (version != VPC_FORMAT_VERSION)   return false;
    if (cached_cvh != vexi_compiler_version_hash()) return false;
    if (dep_count > 100000)              return false; // sanity

    out_deps.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        uint32_t path_len = 0;
        if (!read_u32_le(buf.data(), buf.size(), off, path_len)) return false;
        if (path_len > 32768)            return false;
        if (off + path_len > buf.size()) return false;
        ProjectCacheDep d;
        d.path.assign(reinterpret_cast<const char *>(buf.data() + off), path_len);
        off += path_len;
        if (!read_u64_le(buf.data(), buf.size(), off, d.source_hash)) return false;
        out_deps.push_back(std::move(d));
    }

    uint32_t velb_size = 0;
    if (!read_u32_le(buf.data(), buf.size(), off, velb_size)) return false;
    if (off + velb_size > buf.size())                          return false;
    out_velb.assign(buf.data() + off, buf.data() + off + velb_size);
    return true;
}

bool project_cache_save(const std::string &cache_path,
                         uint32_t opts_hash,
                         const std::vector<ProjectCacheDep> &deps,
                         const std::vector<uint8_t> &velb) {
    std::vector<uint8_t> buf;
    // Reserva pesimista para evitar reallocs.
    size_t est = 16;
    for (const auto &d : deps) est += 4 + d.path.size() + 8;
    est += 4 + velb.size();
    buf.reserve(est);

    write_u32_le(buf, VPC_MAGIC);
    write_u16_le(buf, VPC_FORMAT_VERSION);
    write_u16_le(buf, 0);  // reserved
    write_u32_le(buf, opts_hash);
    // Phase M.L15: compiler_version_hash u64.  Si el compiler cambia
    // (cambio de version, reglas de mangling, ABI invariantes), el
    // cache se invalida automaticamente.  Reusa el mismo hash que
    // vexi_format.h usa para verificacion del .vexi (M5.b).
    write_u64_le(buf, vexi_compiler_version_hash());
    write_u32_le(buf, static_cast<uint32_t>(deps.size()));
    for (const auto &d : deps) {
        write_u32_le(buf, static_cast<uint32_t>(d.path.size()));
        buf.insert(buf.end(), d.path.begin(), d.path.end());
        write_u64_le(buf, d.source_hash);
    }
    write_u32_le(buf, static_cast<uint32_t>(velb.size()));
    buf.insert(buf.end(), velb.begin(), velb.end());

    return write_file_atomic_internal(cache_path, buf);
}

bool project_cache_validate(const std::vector<ProjectCacheDep> &cached_deps) {
    // Para cada dep, abre el path en disco, calcula FNV-1a 64 del source
    // y compara con el cacheado.  Si CUALQUIER archivo no existe o el
    // hash difiere, cache miss.
    for (const auto &d : cached_deps) {
        std::vector<uint8_t> bytes;
        if (!read_file_bytes_internal(d.path, bytes)) return false;
        const uint64_t h = fnv1a64_bytes(bytes.data(), bytes.size());
        if (h != d.source_hash) return false;
    }
    return true;
}

} // namespace vex
