/**
 * @file vexi_format.cpp
 * @brief Implementacion del formato `.vexi` (Phase M.2).
 *
 * Layout binario detallado:
 *
 *   Header (40 bytes en v2; 32 bytes en v1):
 *     +0   u32  magic
 *     +4   u16  format_version
 *     +6   u16  _reserved
 *     +8   u64  abi_hash
 *     +16  u64  source_hash
 *     +24  u64  compiler_version_hash  (v2+)  cierra L.27
 *     +32  u32  symbol_count
 *     +36  u32  string_pool_offset
 *
 *   Symbol entry (variable, alineado a 4):
 *     +0   u8   kind (VexiSymbolKind)
 *     +1   u8   flags (bit 0 = is_opaque, bit 1 = is_extern)
 *     +2   u16  align_override (typedef new)
 *     +4   u32  name_off              (offset en string pool)
 *     +8   u32  name_len
 *     +12  u32  payload_off           (offset al payload variable)
 *     +16  u32  payload_len           (longitud del payload en bytes)
 *
 *   Payload per-kind (en el bloque de payload contiguo):
 *
 *     TYPEDEF_ALIAS / TYPEDEF_NEW (16+ bytes):
 *       +0   u64  nominal_abi
 *       +8   u32  underlying_off
 *       +12  u32  underlying_len
 *
 *     FUNCTION (16 + variable):
 *       +0   u32  ret_type_off
 *       +4   u32  ret_type_len
 *       +8   u32  param_count
 *       +12  u32  extern_lib_off (0 si !is_extern)
 *       +16  ParamSlot[param_count] (8 bytes c/u: u32 type_off, u16 type_len, u16 name_off_packed/len)
 *
 *     STRUCT / CLASS (variable):
 *       +0   u32  super_off    (0 si STRUCT)
 *       +4   u32  super_len    (0 si STRUCT)
 *       +8   u32  size_bytes
 *       +12  u32  align_bytes
 *       +16  u32  field_count
 *       +20  u32  iface_count  (siempre 0 para STRUCT)
 *       +24  FieldSlot[field_count] (24 bytes c/u)
 *       +    NameSlot[iface_count]  (8 bytes c/u)
 *
 *     ENUM (variable):
 *       +0   u32  variant_count
 *       +4   VariantSlot[variant_count] (16 bytes c/u: tag, name_off, name_len, payload_count, payload_off_off, payload_off_len)
 *       +    PayloadType[]   (entries de 8 bytes: off + len)
 *
 *   String pool (al final):
 *     bytes raw concatenados de cada nombre + tipo unique.  Sin null
 *     terminators (longitud va en el slot).  Dedup: cada string que se
 *     reuse apunta al mismo offset.
 *
 * Coste runtime estimado del emit/parse: ~5 us por modulo con 20 simbolos.
 * Despreciable comparado con el lex+parse del .vex original (~3-5 ms).
 */

#include "vex/vexi_format.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace vex {

// ---------------------------------------------------------------------------
// FNV-1a 64-bit.  Identica formula que ModuleGraph::fnv1a_; replicada
// aqui para evitar dep circular module_resolver -> vexi_format.
// ---------------------------------------------------------------------------
uint64_t vexi_fnv1a(const void *data, size_t len) noexcept {
    constexpr uint64_t OFFSET = 0xCBF29CE484222325ULL;
    constexpr uint64_t PRIME  = 0x100000001B3ULL;
    const auto *p = static_cast<const uint8_t *>(data);
    uint64_t h = OFFSET;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= PRIME;
    }
    return h;
}

// ===========================================================================
// String pool builder con dedup.  Cada string se anyade una sola vez;
// segundo uso devuelve el mismo offset.
// ===========================================================================
class StringPoolBuilder {
public:
    StringPoolBuilder() {
        // Reservar primer slot para "string vacio" (offset 0, len 0).
        // Esto permite usar offset=0 como sentinel "sin string" en
        // campos opcionales (super_class de struct, etc.).
        offsets_[""] = 0;
    }

    /// Devuelve el offset del string en el pool.  Si no estaba, lo anyade.
    uint32_t intern(const std::string &s) {
        if (s.empty()) return 0;
        auto it = offsets_.find(s);
        if (it != offsets_.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(buf_.size());
        offsets_.emplace(s, off);
        buf_.insert(buf_.end(), s.begin(), s.end());
        return off;
    }

    const std::vector<uint8_t> &bytes() const noexcept { return buf_; }
    size_t size() const noexcept { return buf_.size(); }

private:
    std::vector<uint8_t>                          buf_;
    std::unordered_map<std::string, uint32_t>     offsets_;
};

// ---------------------------------------------------------------------------
// Helpers de escritura little-endian a un buffer.
// ---------------------------------------------------------------------------
static inline void write_u8(std::vector<uint8_t> &b, uint8_t v) {
    b.push_back(v);
}
static inline void write_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
static inline void write_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}
static inline void write_u64(std::vector<uint8_t> &b, uint64_t v) {
    write_u32(b, static_cast<uint32_t>(v & 0xFFFFFFFFull));
    write_u32(b, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

// ---------------------------------------------------------------------------
// Helpers de lectura little-endian con bounds check.  Devuelven false si
// el read excede el buffer.
// ---------------------------------------------------------------------------
static inline bool read_u8(const uint8_t *p, size_t cap, size_t &off, uint8_t &v) {
    if (off + 1 > cap) return false;
    v = p[off];
    off += 1;
    return true;
}
static inline bool read_u16(const uint8_t *p, size_t cap, size_t &off, uint16_t &v) {
    if (off + 2 > cap) return false;
    v = static_cast<uint16_t>(p[off])
      | (static_cast<uint16_t>(p[off+1]) << 8);
    off += 2;
    return true;
}
static inline bool read_u32(const uint8_t *p, size_t cap, size_t &off, uint32_t &v) {
    if (off + 4 > cap) return false;
    v = static_cast<uint32_t>(p[off])
      | (static_cast<uint32_t>(p[off+1]) << 8)
      | (static_cast<uint32_t>(p[off+2]) << 16)
      | (static_cast<uint32_t>(p[off+3]) << 24);
    off += 4;
    return true;
}
static inline bool read_u64(const uint8_t *p, size_t cap, size_t &off, uint64_t &v) {
    uint32_t lo = 0, hi = 0;
    if (!read_u32(p, cap, off, lo)) return false;
    if (!read_u32(p, cap, off, hi)) return false;
    v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}

// ===========================================================================
// Emitter: serializa VexiModule -> bytes.
// ===========================================================================
//
// Estrategia:
//   1. Construir string pool.
//   2. Emitir payloads variables a un buffer separado.
//   3. Emitir symbol entries (con offsets al pool + payload buffer).
//   4. Anyadir header al inicio + payloads + pool.
//   5. Calcular abi_hash sobre los simbolos serializados (sin header).
//
// El abi_hash incluye los bytes de TODAS las entries en orden + el pool.
// Cualquier cambio en la interfaz publica (nuevo simbolo, cambio de
// firma, etc.) produce un hash distinto.

// Base comun: 16 bytes con nominal_abi + underlying typename.  Usado
// por typedef alias/new y por global var (que extiende con init_value).
static void emit_typedef_base(std::vector<uint8_t> &payload,
                               StringPoolBuilder &pool,
                               const VexiSymbol &sym) {
    write_u64(payload, sym.nominal_abi);
    const uint32_t und_off = pool.intern(sym.underlying_type);
    write_u32(payload, und_off);
    write_u32(payload, static_cast<uint32_t>(sym.underlying_type.size()));
}

static void emit_payload_for_typedef(std::vector<uint8_t> &payload,
                                      StringPoolBuilder &pool,
                                      const VexiSymbol &sym) {
    emit_typedef_base(payload, pool, sym);
    // Phase M.L8: bloque {explicit from/to T;}.  Para TYPEDEF_ALIAS el
    // bloque siempre esta vacio (cuenta 0); para TYPEDEF_NEW puede llevar
    // N entries.  Layout: u32 from_count + u32 to_count + entries en
    // orden (cada entry: type_off u32 + type_len u32 + is_public u8 + 3 pad).
    write_u32(payload, static_cast<uint32_t>(sym.from_conversions.size()));
    write_u32(payload, static_cast<uint32_t>(sym.to_conversions.size()));
    auto emit_conv = [&](const VexiSymbol::ExplicitConvEntry &c) {
        const uint32_t t_off = pool.intern(c.type_str);
        write_u32(payload, t_off);
        write_u32(payload, static_cast<uint32_t>(c.type_str.size()));
        write_u8(payload, c.is_public ? 1u : 0u);
        write_u8(payload, 0); write_u8(payload, 0); write_u8(payload, 0);
    };
    for (const auto &c : sym.from_conversions) emit_conv(c);
    for (const auto &c : sym.to_conversions)   emit_conv(c);
}

// .vexi v4: GLOBAL_VAR.  Layout:
//   +0   typedef_base (16 bytes: nominal_abi u64 + und_off u32 + und_len u32)
//   +16  has_init_value u8 + 7 pad
//   +24  init_value u64
//   +32  has_blob_ref u8 + blob_kind_hint u8 + attr_flags u8 + 1 pad
//   +36  attr_align u16 + 2 pad
//   +40  blob_offset u32
//   +44  attr_section_off u32
//   +48  attr_section_len u32
//   = 52 bytes (multiplo de 4)
static void emit_payload_for_global(std::vector<uint8_t> &payload,
                                     StringPoolBuilder &pool,
                                     const VexiSymbol &sym) {
    emit_typedef_base(payload, pool, sym);   // 16 bytes
    write_u8(payload, sym.has_init_value ? 1u : 0u);
    write_u8(payload, 0); write_u8(payload, 0); write_u8(payload, 0);
    write_u8(payload, 0); write_u8(payload, 0); write_u8(payload, 0);
    write_u8(payload, 0);   // 7 bytes pad para alinear el u64
    write_u32(payload, static_cast<uint32_t>(sym.init_value & 0xFFFFFFFFull));
    write_u32(payload, static_cast<uint32_t>((sym.init_value >> 32) & 0xFFFFFFFFull));
    // v4: blob ref + atributos.
    write_u8(payload, sym.has_blob_ref ? 1u : 0u);
    write_u8(payload, sym.blob_kind_hint);
    write_u8(payload, sym.attr_flags);
    write_u8(payload, 0);
    write_u16(payload, sym.attr_align);
    write_u8(payload, 0); write_u8(payload, 0);
    write_u32(payload, sym.blob_offset);
    const uint32_t sec_off = sym.attr_section.empty()
                                ? 0u
                                : pool.intern(sym.attr_section);
    write_u32(payload, sec_off);
    write_u32(payload, static_cast<uint32_t>(sym.attr_section.size()));
}

static void emit_payload_for_function(std::vector<uint8_t> &payload,
                                       StringPoolBuilder &pool,
                                       const VexiSymbol &sym) {
    const uint32_t ret_off = pool.intern(sym.return_type);
    write_u32(payload, ret_off);
    write_u32(payload, static_cast<uint32_t>(sym.return_type.size()));
    write_u32(payload, static_cast<uint32_t>(sym.param_types.size()));
    const uint32_t lib_off = sym.is_extern ? pool.intern(sym.extern_lib) : 0;
    write_u32(payload, lib_off);
    // Phase M5.b L.5: lib_len explicito (sustituye el hack de truncar al
    // primer NUL o a 256 bytes en versiones previas).
    write_u32(payload, sym.is_extern
                          ? static_cast<uint32_t>(sym.extern_lib.size())
                          : 0u);
    // Phase M.5: mangled_label (8 bytes: off + len; vacio = mismo que name).
    const uint32_t ml_off = pool.intern(sym.mangled_label);
    write_u32(payload, ml_off);
    write_u32(payload, static_cast<uint32_t>(sym.mangled_label.size()));
    // ParamSlot: type_off (u32) + type_len (u32) + name_off (u32) + name_len (u32).
    // 16 bytes c/u en lugar de 8 (preferimos claridad; el coste es despreciable).
    const size_t n = sym.param_types.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t t_off = pool.intern(sym.param_types[i]);
        write_u32(payload, t_off);
        write_u32(payload, static_cast<uint32_t>(sym.param_types[i].size()));
        std::string nm = (i < sym.param_names.size()) ? sym.param_names[i] : std::string();
        const uint32_t n_off = pool.intern(nm);
        write_u32(payload, n_off);
        write_u32(payload, static_cast<uint32_t>(nm.size()));
    }
}

static void emit_payload_for_struct_or_class(std::vector<uint8_t> &payload,
                                              StringPoolBuilder &pool,
                                              const VexiSymbol &sym) {
    const uint32_t super_off = pool.intern(sym.super_class);
    write_u32(payload, super_off);
    write_u32(payload, static_cast<uint32_t>(sym.super_class.size()));
    write_u32(payload, sym.size_bytes);
    write_u32(payload, sym.align_bytes);
    write_u32(payload, static_cast<uint32_t>(sym.fields.size()));
    write_u32(payload, static_cast<uint32_t>(sym.interfaces.size()));
    // Phase M6.b L.6: method_count antes de las entries.
    write_u32(payload, static_cast<uint32_t>(sym.methods.size()));
    // FieldSlot: name_off+len + type_off+len + offset + size + bit_offset + bit_width + pad.
    // 24 bytes c/u alineados.
    for (const auto &f : sym.fields) {
        const uint32_t no = pool.intern(f.name);
        const uint32_t to = pool.intern(f.type_str);
        write_u32(payload, no);
        write_u32(payload, static_cast<uint32_t>(f.name.size()));
        write_u32(payload, to);
        write_u32(payload, static_cast<uint32_t>(f.type_str.size()));
        write_u32(payload, f.offset);
        write_u16(payload, static_cast<uint16_t>(f.size));
        write_u8(payload, f.bit_offset);
        write_u8(payload, f.bit_width);
    }
    // InterfaceSlot: name_off+len (8 bytes c/u).
    for (const auto &iname : sym.interfaces) {
        const uint32_t no = pool.intern(iname);
        write_u32(payload, no);
        write_u32(payload, static_cast<uint32_t>(iname.size()));
    }
    // MethodSlot: name + return_type + param_count + vtable_index + flags
    //             + mangled_label + param_types[].  Variable length.
    for (const auto &m : sym.methods) {
        const uint32_t no = pool.intern(m.name);
        const uint32_t ro = pool.intern(m.return_type);
        const uint32_t ml = pool.intern(m.mangled_label);
        write_u32(payload, no);
        write_u32(payload, static_cast<uint32_t>(m.name.size()));
        write_u32(payload, ro);
        write_u32(payload, static_cast<uint32_t>(m.return_type.size()));
        write_u32(payload, static_cast<uint32_t>(m.param_types.size()));
        write_u32(payload, m.vtable_index);
        write_u32(payload, static_cast<uint32_t>(m.flags));
        write_u32(payload, ml);
        write_u32(payload, static_cast<uint32_t>(m.mangled_label.size()));
        // Param types inline.
        for (const auto &pt : m.param_types) {
            const uint32_t to = pool.intern(pt);
            write_u32(payload, to);
            write_u32(payload, static_cast<uint32_t>(pt.size()));
        }
    }
}

static void emit_payload_for_enum(std::vector<uint8_t> &payload,
                                   StringPoolBuilder &pool,
                                   const VexiSymbol &sym) {
    // v5: size_bytes + align_bytes al inicio para que el consumer
    // pueda allocar slots correctamente.  Antes de v5 el consumer
    // veia size_bytes=0 y allocaba slots de 0 bytes -> corrupcion.
    write_u32(payload, sym.size_bytes);
    write_u32(payload, sym.align_bytes);
    write_u32(payload, static_cast<uint32_t>(sym.variants.size()));
    // VariantSlot: tag + name_off+len + payload_count + payload entries inline.
    for (const auto &v : sym.variants) {
        write_u32(payload, v.tag);
        const uint32_t no = pool.intern(v.name);
        write_u32(payload, no);
        write_u32(payload, static_cast<uint32_t>(v.name.size()));
        write_u32(payload, static_cast<uint32_t>(v.payload_types.size()));
        for (const auto &pt : v.payload_types) {
            const uint32_t to = pool.intern(pt);
            write_u32(payload, to);
            write_u32(payload, static_cast<uint32_t>(pt.size()));
        }
    }
}

std::vector<uint8_t> vexi_emit(const VexiModule &mod) {
    // 1. Construir string pool + payloads contiguos.
    StringPoolBuilder pool;
    std::vector<uint8_t> payloads;
    payloads.reserve(mod.symbols.size() * 32);

    struct SymOffset {
        uint32_t name_off;
        uint32_t name_len;
        uint32_t payload_off;
        uint32_t payload_len;
        uint8_t  flags;
        uint16_t align_override;
    };
    std::vector<SymOffset> sym_offsets;
    sym_offsets.reserve(mod.symbols.size());

    for (const auto &sym : mod.symbols) {
        SymOffset so{};
        so.name_off       = pool.intern(sym.name);
        so.name_len       = static_cast<uint32_t>(sym.name.size());
        so.payload_off    = static_cast<uint32_t>(payloads.size());
        so.flags          = 0;
        if (sym.is_opaque) so.flags |= 0x01;
        if (sym.is_extern) so.flags |= 0x02;
        if (sym.is_const)  so.flags |= 0x04;   // L.7: GLOBAL_VAR const
        so.align_override = sym.align_override;

        switch (sym.kind) {
            case VexiSymbolKind::TYPEDEF_ALIAS:
            case VexiSymbolKind::TYPEDEF_NEW:
                emit_payload_for_typedef(payloads, pool, sym);
                break;
            case VexiSymbolKind::FUNCTION:
                emit_payload_for_function(payloads, pool, sym);
                break;
            case VexiSymbolKind::STRUCT:
            case VexiSymbolKind::CLASS:
                emit_payload_for_struct_or_class(payloads, pool, sym);
                break;
            case VexiSymbolKind::ENUM:
                emit_payload_for_enum(payloads, pool, sym);
                break;
            case VexiSymbolKind::GLOBAL_VAR:
                // Phase M.L7: extiende typedef payload con init_value
                // para que el lowering pueda inline-ar const numericas.
                emit_payload_for_global(payloads, pool, sym);
                break;
        }
        so.payload_len = static_cast<uint32_t>(payloads.size()) - so.payload_off;
        sym_offsets.push_back(so);
    }

    // 2. Construir el buffer final: header + symbol entries + payloads + deps
    //    + blob_pool + string_pool.
    // .vexi v4: header crece de 48 a 64 bytes (anyade blob_pool_offset u32 +
    // blob_pool_size u32 + blob_pool_alignment u8 + 7 pad en offsets 48..63).
    const size_t HEADER_BYTES   = 64;
    const size_t SYMENTRY_BYTES = 20;     // 1 + 1 + 2 + 4 + 4 + 4 + 4
    const size_t DEP_ENTRY_BYTES = 16;    // u32 name_off + u32 name_len + u64 abi_hash
    const size_t entries_bytes  = mod.symbols.size() * SYMENTRY_BYTES;
    const size_t payloads_bytes = payloads.size();
    const size_t deps_bytes     = mod.deps.size() * DEP_ENTRY_BYTES;
    const size_t blob_pool_bytes = mod.blob_pool.size();
    const size_t pool_bytes     = pool.bytes().size();

    // Pre-internamos los nombres de los deps al pool ANTES de calcular
    // offsets (asi el pool_start no cambia tras emit de la dep table).
    std::vector<std::pair<uint32_t, uint32_t>> dep_name_offs;
    dep_name_offs.reserve(mod.deps.size());
    for (const auto &d : mod.deps) {
        dep_name_offs.emplace_back(pool.intern(d.name),
                                    static_cast<uint32_t>(d.name.size()));
    }

    std::vector<uint8_t> out;
    out.reserve(HEADER_BYTES + entries_bytes + payloads_bytes
                + deps_bytes + blob_pool_bytes + pool_bytes + 8);

    // Header placeholder; rellenamos al final con offsets correctos.
    out.resize(HEADER_BYTES, 0);

    // Symbol entries (todos consecutivos).
    const uint32_t entries_start = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < mod.symbols.size(); ++i) {
        const auto &sym = mod.symbols[i];
        const auto &so  = sym_offsets[i];
        write_u8(out, static_cast<uint8_t>(sym.kind));
        write_u8(out, so.flags);
        write_u16(out, so.align_override);
        write_u32(out, so.name_off);
        write_u32(out, so.name_len);
        write_u32(out, so.payload_off);
        write_u32(out, so.payload_len);
    }

    // Payloads contiguos tras las entries.
    const uint32_t payloads_start = static_cast<uint32_t>(out.size());
    out.insert(out.end(), payloads.begin(), payloads.end());

    // Phase M4.ext L.13: dep table tras los payloads, antes del blob_pool.
    const uint32_t deps_start = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < mod.deps.size(); ++i) {
        write_u32(out, dep_name_offs[i].first);   // name_off (rel al pool)
        write_u32(out, dep_name_offs[i].second);  // name_len
        // abi_hash u64 little-endian
        const uint64_t h = mod.deps[i].abi_hash;
        write_u32(out, static_cast<uint32_t>(h & 0xFFFFFFFFull));
        write_u32(out, static_cast<uint32_t>((h >> 32) & 0xFFFFFFFFull));
    }

    // v4: blob_pool tras la dep table.  Alineado a 8 bytes para que los
    // BlobHeaders dentro mantengan su alineamiento absoluto al inicio del
    // fichero (asumiendo HEADER_BYTES=64 + entries+payloads+deps son ya
    // multiplos de 4).  Pad explicito al multiplo de 8.
    while (out.size() % 8 != 0) out.push_back(0);
    const uint32_t blob_pool_start = static_cast<uint32_t>(out.size());
    if (!mod.blob_pool.empty()) {
        out.insert(out.end(), mod.blob_pool.begin(), mod.blob_pool.end());
    }

    // String pool al final.
    const uint32_t pool_start = static_cast<uint32_t>(out.size());
    out.insert(out.end(), pool.bytes().begin(), pool.bytes().end());

    // 3. Re-escribir header con los valores reales.
    auto patch_u32 = [&](size_t pos, uint32_t v) {
        out[pos+0] = static_cast<uint8_t>(v & 0xFFu);
        out[pos+1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        out[pos+2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        out[pos+3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto patch_u64 = [&](size_t pos, uint64_t v) {
        patch_u32(pos, static_cast<uint32_t>(v & 0xFFFFFFFFull));
        patch_u32(pos+4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
    };
    patch_u32(0, VEXI_MAGIC);
    patch_u32(4, static_cast<uint16_t>(VEXI_FORMAT_VERSION)
              | (static_cast<uint32_t>(0) << 16));
    // 4. abi_hash: FNV-1a sobre entries + payloads + pool (todo despues del header).
    const uint64_t abi_hash = vexi_fnv1a(
        out.data() + HEADER_BYTES,
        out.size() - HEADER_BYTES);
    patch_u64(8,  abi_hash);
    patch_u64(16, mod.source_hash);
    // Phase M5.b L.27: compiler_version_hash en offset 24.  Si el caller
    // ya lo seteo en mod (testing), respetamos su valor; si no, usamos
    // el del compilador actual.
    const uint64_t cvh = (mod.compiler_version_hash != 0)
        ? mod.compiler_version_hash
        : vexi_compiler_version_hash();
    patch_u64(24, cvh);

    // symbol_count + string_pool_offset (relativo al inicio del fichero).
    patch_u32(32, static_cast<uint32_t>(mod.symbols.size()));
    patch_u32(36, pool_start);
    // Phase M4.ext L.13: dep_count + dep_table_offset (offset al inicio
    // del bloque de DepEntry, 16 bytes c/u en formato u32+u32+u64).
    patch_u32(40, static_cast<uint32_t>(mod.deps.size()));
    patch_u32(44, deps_start);
    // v4: blob_pool_offset + blob_pool_size + blob_pool_alignment + pad.
    patch_u32(48, blob_pool_start);
    patch_u32(52, static_cast<uint32_t>(mod.blob_pool.size()));
    out[56] = mod.blob_pool_alignment;
    out[57] = 0; out[58] = 0; out[59] = 0;
    out[60] = 0; out[61] = 0; out[62] = 0; out[63] = 0;

    // Adjustar payload_off de cada entry: hasta ahora son relativos al
    // BLOQUE de payloads (empieza en 0).  Sumar payloads_start para que
    // sean offsets absolutos desde el inicio del fichero.
    for (size_t i = 0; i < mod.symbols.size(); ++i) {
        const size_t entry_pos = entries_start + i * SYMENTRY_BYTES;
        const size_t poff_pos  = entry_pos + 12;  // tras kind+flags+align+name_off+name_len
        uint32_t cur = 0;
        cur  = static_cast<uint32_t>(out[poff_pos+0])
             | (static_cast<uint32_t>(out[poff_pos+1]) << 8)
             | (static_cast<uint32_t>(out[poff_pos+2]) << 16)
             | (static_cast<uint32_t>(out[poff_pos+3]) << 24);
        cur += payloads_start;
        patch_u32(poff_pos, cur);
    }
    // Similarmente, todos los name_off + offsets dentro de payloads
    // apuntan al pool; los emitimos ya con offset relativo al pool.
    // El parser sabe sumar pool_start (= string_pool_offset del header)
    // para obtener offset absoluto al pool.  Convencion: en disco, los
    // *_off de strings se almacenan RELATIVOS al inicio del pool (no
    // absolutos al fichero) para simplificar el parser.

    return out;
}

// ===========================================================================
// Parser: bytes -> VexiModule.
// ===========================================================================

static bool read_name(const uint8_t *data, size_t size,
                       uint32_t name_off, uint32_t name_len,
                       uint32_t pool_start, std::string &out) {
    const size_t abs = static_cast<size_t>(pool_start) + name_off;
    if (abs + name_len > size) return false;
    out.assign(reinterpret_cast<const char *>(data) + abs, name_len);
    return true;
}

static bool parse_payload_typedef(const uint8_t *data, size_t size,
                                   uint32_t payload_off, uint32_t payload_len,
                                   uint32_t pool_start,
                                   VexiSymbol &out) {
    if (payload_len < 16) return false;
    size_t off = payload_off;
    if (!read_u64(data, size, off, out.nominal_abi)) return false;
    uint32_t und_off = 0, und_len = 0;
    if (!read_u32(data, size, off, und_off)) return false;
    if (!read_u32(data, size, off, und_len)) return false;
    if (!read_name(data, size, und_off, und_len, pool_start, out.underlying_type)) {
        return false;
    }
    // Phase M.L8: bloque {from/to} conversions.  Si payload no incluye
    // estos campos (compat con .vexi v2 viejos), retornar OK con listas
    // vacias.  El offset actual debe caber dentro de payload_off+payload_len.
    if (off + 8 > payload_off + payload_len) {
        return true;
    }
    uint32_t from_count = 0, to_count = 0;
    if (!read_u32(data, size, off, from_count)) return false;
    if (!read_u32(data, size, off, to_count))   return false;
    if (from_count > 1000 || to_count > 1000) return false;  // sanity
    auto read_conv = [&](VexiSymbol::ExplicitConvEntry &c) -> bool {
        uint32_t t_off = 0, t_len = 0;
        if (!read_u32(data, size, off, t_off)) return false;
        if (!read_u32(data, size, off, t_len)) return false;
        uint8_t pub = 0;
        if (!read_u8(data, size, off, pub))    return false;
        // skip 3 bytes pad
        off += 3;
        c.is_public = (pub != 0);
        return read_name(data, size, t_off, t_len, pool_start, c.type_str);
    };
    out.from_conversions.resize(from_count);
    for (uint32_t i = 0; i < from_count; ++i) {
        if (!read_conv(out.from_conversions[i])) return false;
    }
    out.to_conversions.resize(to_count);
    for (uint32_t i = 0; i < to_count; ++i) {
        if (!read_conv(out.to_conversions[i])) return false;
    }
    return true;
}

// Phase M.L7: parse del payload extendido para GLOBAL_VAR.  Layout:
//   bytes 0-15  : nominal_abi (u64) + und_off (u32) + und_len (u32)
//   bytes 16-31 : has_init_value (u8) + 7 pad + init_value (u64)
static bool parse_payload_global(const uint8_t *data, size_t size,
                                  uint32_t payload_off, uint32_t payload_len,
                                  uint32_t pool_start,
                                  VexiSymbol &out) {
    if (payload_len < 16) return false;
    size_t off = payload_off;
    if (!read_u64(data, size, off, out.nominal_abi)) return false;
    uint32_t und_off = 0, und_len = 0;
    if (!read_u32(data, size, off, und_off)) return false;
    if (!read_u32(data, size, off, und_len)) return false;
    if (!read_name(data, size, und_off, und_len, pool_start, out.underlying_type)) {
        return false;
    }
    // Si hay extras (>=16 bytes mas), leer init.  Compat con .vexi viejos:
    // si solo hay 16 bytes, asumir has_init_value=false.
    if (payload_len < 16 + 16) {
        return true;
    }
    uint8_t has = 0;
    if (!read_u8(data, size, off, has)) return false;
    off += 7;   // skip pad
    uint32_t lo = 0, hi = 0;
    if (!read_u32(data, size, off, lo)) return false;
    if (!read_u32(data, size, off, hi)) return false;
    out.has_init_value = (has != 0);
    out.init_value     = static_cast<uint64_t>(lo)
                       | (static_cast<uint64_t>(hi) << 32);
    // v4: campos extra (blob ref + atributos).  Total payload >= 52.
    if (payload_len < 52) {
        return true;
    }
    uint8_t hbr = 0, bkh = 0, af = 0, _p0 = 0;
    uint16_t align = 0, _p1 = 0;
    uint32_t blob_off = 0, sec_off = 0, sec_len = 0;
    if (!read_u8 (data, size, off, hbr)) return false;
    if (!read_u8 (data, size, off, bkh)) return false;
    if (!read_u8 (data, size, off, af))  return false;
    if (!read_u8 (data, size, off, _p0)) return false;
    if (!read_u16(data, size, off, align)) return false;
    if (!read_u16(data, size, off, _p1)) return false;
    if (!read_u32(data, size, off, blob_off)) return false;
    if (!read_u32(data, size, off, sec_off)) return false;
    if (!read_u32(data, size, off, sec_len)) return false;
    out.has_blob_ref    = (hbr != 0);
    out.blob_kind_hint  = bkh;
    out.attr_flags      = af;
    out.attr_align      = align;
    out.blob_offset     = blob_off;
    if (sec_len > 0) {
        if (!read_name(data, size, sec_off, sec_len, pool_start, out.attr_section)) {
            return false;
        }
    }
    return true;
}

static bool parse_payload_function(const uint8_t *data, size_t size,
                                    uint32_t payload_off, uint32_t payload_len,
                                    uint32_t pool_start,
                                    VexiSymbol &out) {
    // Phase M5.b L.5: format v2 anyade lib_len explicito.  Total fixed
    // bytes en payload = 28 (vs 24 en v1): ret_off+ret_len+pc+
    // lib_off+lib_len+ml_off+ml_len = 7 u32 = 28 bytes.
    if (payload_len < 28) return false;
    size_t off = payload_off;
    uint32_t ret_off=0, ret_len=0, pc=0, lib_off=0, lib_len=0;
    uint32_t ml_off=0, ml_len=0;
    if (!read_u32(data, size, off, ret_off)) return false;
    if (!read_u32(data, size, off, ret_len)) return false;
    if (!read_u32(data, size, off, pc)) return false;
    if (!read_u32(data, size, off, lib_off)) return false;
    if (!read_u32(data, size, off, lib_len)) return false;
    if (!read_u32(data, size, off, ml_off)) return false;
    if (!read_u32(data, size, off, ml_len)) return false;
    if (ml_len > 0) {
        if (!read_name(data, size, ml_off, ml_len, pool_start, out.mangled_label))
            return false;
    }
    if (!read_name(data, size, ret_off, ret_len, pool_start, out.return_type)) return false;
    if (out.is_extern && lib_off > 0 && lib_len > 0) {
        // Phase M5.b L.5: lib_len explicito (format v2).  Cero ambiguedad
        // sobre donde termina el nombre, sin hack de cap 256 bytes.
        if (!read_name(data, size, lib_off, lib_len, pool_start, out.extern_lib))
            return false;
    }
    out.param_types.reserve(pc);
    out.param_names.reserve(pc);
    for (uint32_t i = 0; i < pc; ++i) {
        uint32_t t_off=0, t_len=0, n_off=0, n_len=0;
        if (!read_u32(data, size, off, t_off)) return false;
        if (!read_u32(data, size, off, t_len)) return false;
        if (!read_u32(data, size, off, n_off)) return false;
        if (!read_u32(data, size, off, n_len)) return false;
        std::string tnm;
        if (!read_name(data, size, t_off, t_len, pool_start, tnm)) return false;
        std::string nnm;
        if (!read_name(data, size, n_off, n_len, pool_start, nnm)) return false;
        out.param_types.push_back(std::move(tnm));
        out.param_names.push_back(std::move(nnm));
    }
    return true;
}

static bool parse_payload_struct_or_class(const uint8_t *data, size_t size,
                                           uint32_t payload_off, uint32_t payload_len,
                                           uint32_t pool_start,
                                           VexiSymbol &out) {
    // Phase M6.b L.6: payload header crece a 28 bytes fijos (anyade method_count).
    if (payload_len < 28) return false;
    size_t off = payload_off;
    uint32_t super_off=0, super_len=0, fc=0, ic=0, mc=0;
    if (!read_u32(data, size, off, super_off)) return false;
    if (!read_u32(data, size, off, super_len)) return false;
    if (!read_u32(data, size, off, out.size_bytes)) return false;
    if (!read_u32(data, size, off, out.align_bytes)) return false;
    if (!read_u32(data, size, off, fc)) return false;
    if (!read_u32(data, size, off, ic)) return false;
    if (!read_u32(data, size, off, mc)) return false;
    if (super_len > 0) {
        if (!read_name(data, size, super_off, super_len, pool_start, out.super_class))
            return false;
    }
    out.fields.reserve(fc);
    for (uint32_t i = 0; i < fc; ++i) {
        VexiSymbol::FieldInfo fi{};
        uint32_t n_off=0, n_len=0, t_off=0, t_len=0;
        uint16_t fsize = 0;
        uint8_t  bo=0, bw=0;
        if (!read_u32(data, size, off, n_off)) return false;
        if (!read_u32(data, size, off, n_len)) return false;
        if (!read_u32(data, size, off, t_off)) return false;
        if (!read_u32(data, size, off, t_len)) return false;
        if (!read_u32(data, size, off, fi.offset)) return false;
        if (!read_u16(data, size, off, fsize)) return false;
        if (!read_u8(data, size, off, bo)) return false;
        if (!read_u8(data, size, off, bw)) return false;
        fi.size       = fsize;
        fi.bit_offset = bo;
        fi.bit_width  = bw;
        if (!read_name(data, size, n_off, n_len, pool_start, fi.name)) return false;
        if (!read_name(data, size, t_off, t_len, pool_start, fi.type_str)) return false;
        out.fields.push_back(std::move(fi));
    }
    out.interfaces.reserve(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        uint32_t n_off=0, n_len=0;
        if (!read_u32(data, size, off, n_off)) return false;
        if (!read_u32(data, size, off, n_len)) return false;
        std::string nm;
        if (!read_name(data, size, n_off, n_len, pool_start, nm)) return false;
        out.interfaces.push_back(std::move(nm));
    }
    // Phase M6.b L.6: methods.
    out.methods.reserve(mc);
    for (uint32_t i = 0; i < mc; ++i) {
        VexiSymbol::MethodInfo m{};
        uint32_t n_off=0, n_len=0, r_off=0, r_len=0, pc=0, vti=0, fl=0, ml_off=0, ml_len=0;
        if (!read_u32(data, size, off, n_off)) return false;
        if (!read_u32(data, size, off, n_len)) return false;
        if (!read_u32(data, size, off, r_off)) return false;
        if (!read_u32(data, size, off, r_len)) return false;
        if (!read_u32(data, size, off, pc)) return false;
        if (!read_u32(data, size, off, vti)) return false;
        if (!read_u32(data, size, off, fl)) return false;
        if (!read_u32(data, size, off, ml_off)) return false;
        if (!read_u32(data, size, off, ml_len)) return false;
        m.vtable_index = vti;
        m.flags        = static_cast<uint8_t>(fl);
        if (!read_name(data, size, n_off, n_len, pool_start, m.name)) return false;
        if (!read_name(data, size, r_off, r_len, pool_start, m.return_type)) return false;
        if (ml_len > 0) {
            if (!read_name(data, size, ml_off, ml_len, pool_start, m.mangled_label))
                return false;
        }
        m.param_types.reserve(pc);
        for (uint32_t j = 0; j < pc; ++j) {
            uint32_t t_off=0, t_len=0;
            if (!read_u32(data, size, off, t_off)) return false;
            if (!read_u32(data, size, off, t_len)) return false;
            std::string pt;
            if (!read_name(data, size, t_off, t_len, pool_start, pt)) return false;
            m.param_types.push_back(std::move(pt));
        }
        out.methods.push_back(std::move(m));
    }
    return true;
}

static bool parse_payload_enum(const uint8_t *data, size_t size,
                                uint32_t payload_off, uint32_t payload_len,
                                uint32_t pool_start,
                                VexiSymbol &out) {
    if (payload_len < 12) return false;     // v5: size+align+variants
    size_t off = payload_off;
    // v5: size_bytes + align_bytes al inicio.
    if (!read_u32(data, size, off, out.size_bytes)) return false;
    if (!read_u32(data, size, off, out.align_bytes)) return false;
    uint32_t vc = 0;
    if (!read_u32(data, size, off, vc)) return false;
    out.variants.reserve(vc);
    for (uint32_t i = 0; i < vc; ++i) {
        VexiSymbol::EnumVariant v{};
        uint32_t n_off=0, n_len=0, pc=0;
        if (!read_u32(data, size, off, v.tag)) return false;
        if (!read_u32(data, size, off, n_off)) return false;
        if (!read_u32(data, size, off, n_len)) return false;
        if (!read_u32(data, size, off, pc)) return false;
        if (!read_name(data, size, n_off, n_len, pool_start, v.name)) return false;
        v.payload_types.reserve(pc);
        for (uint32_t j = 0; j < pc; ++j) {
            uint32_t t_off=0, t_len=0;
            if (!read_u32(data, size, off, t_off)) return false;
            if (!read_u32(data, size, off, t_len)) return false;
            std::string tnm;
            if (!read_name(data, size, t_off, t_len, pool_start, tnm)) return false;
            v.payload_types.push_back(std::move(tnm));
        }
        out.variants.push_back(std::move(v));
    }
    return true;
}

VexiParseResult vexi_parse(const uint8_t *data, size_t size) {
    VexiParseResult r;
    // .vexi v4: header crece a 64 bytes.
    if (size < 64) {
        r.error_message = "fichero .vexi truncado (< 64 bytes)";
        return r;
    }
    size_t off = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint64_t abi_hash = 0;
    uint64_t source_hash = 0;
    uint64_t cvh = 0;
    uint32_t symbol_count = 0;
    uint32_t pool_start = 0;
    uint32_t dep_count = 0;
    uint32_t dep_table_offset = 0;
    uint32_t blob_pool_offset_hdr = 0;
    uint32_t blob_pool_size_hdr = 0;
    uint8_t  blob_pool_alignment_hdr = 8;
    read_u32(data, size, off, magic);
    read_u16(data, size, off, version);
    read_u16(data, size, off, reserved);
    read_u64(data, size, off, abi_hash);
    read_u64(data, size, off, source_hash);
    read_u64(data, size, off, cvh);          // M5.b L.27
    read_u32(data, size, off, symbol_count);
    read_u32(data, size, off, pool_start);
    read_u32(data, size, off, dep_count);         // M4.ext L.13
    read_u32(data, size, off, dep_table_offset);  // M4.ext L.13
    read_u32(data, size, off, blob_pool_offset_hdr);  // v4
    read_u32(data, size, off, blob_pool_size_hdr);    // v4
    read_u8 (data, size, off, blob_pool_alignment_hdr); // v4
    off += 7;  // pad

    if (magic != VEXI_MAGIC) {
        r.error_message = "magic invalido en .vexi";
        return r;
    }
    if (version != VEXI_FORMAT_VERSION) {
        std::ostringstream os;
        os << "version .vexi no soportada (esperado " << VEXI_FORMAT_VERSION
           << ", encontrado " << version << "); el cache se regenerara";
        r.error_message = os.str();
        return r;
    }
    // Phase M5.b L.27: chequeo del compiler_version_hash.  Si difiere del
    // compilador actual, el .vexi puede haber sido generado por una
    // version distinta con reglas de mangling/ABI diferentes.  Rechazar
    // y forzar regeneracion en el siguiente compile.
    const uint64_t my_cvh = vexi_compiler_version_hash();
    if (cvh != my_cvh) {
        std::ostringstream os;
        os << ".vexi generado por compilador distinto (esperado 0x"
           << std::hex << my_cvh << ", encontrado 0x" << cvh
           << "); el cache se regenerara";
        r.error_message = os.str();
        return r;
    }
    if (symbol_count > 1000000) {
        r.error_message = "symbol_count excesivo (posible corrupcion)";
        return r;
    }
    if (pool_start > size) {
        r.error_message = "string_pool_offset fuera del fichero";
        return r;
    }

    r.module_.format_version        = version;
    r.module_.abi_hash              = abi_hash;
    r.module_.source_hash           = source_hash;
    r.module_.compiler_version_hash = cvh;
    r.module_.symbols.reserve(symbol_count);

    // Symbol entries empiezan en offset 64 (tras el header v4).
    constexpr size_t SYMENTRY_BYTES   = 20;
    constexpr size_t HEADER_BYTES     = 64;
    constexpr size_t DEP_ENTRY_BYTES  = 16;
    // v4: blob_pool extraido a un std::vector para conservar la API
    // existente.  Validamos rangos antes de copiar.
    if (blob_pool_size_hdr != 0) {
        const uint64_t blob_end = static_cast<uint64_t>(blob_pool_offset_hdr)
                                + static_cast<uint64_t>(blob_pool_size_hdr);
        if (blob_pool_offset_hdr < HEADER_BYTES || blob_end > size) {
            r.error_message = "blob_pool fuera del fichero";
            return r;
        }
        r.module_.blob_pool.assign(
            data + blob_pool_offset_hdr,
            data + blob_pool_offset_hdr + blob_pool_size_hdr);
    }
    r.module_.blob_pool_alignment = blob_pool_alignment_hdr;
    if (HEADER_BYTES + symbol_count * SYMENTRY_BYTES > pool_start) {
        r.error_message = "entries exceden el string_pool_offset";
        return r;
    }
    // Validar dep table dentro del fichero.
    if (dep_count > 100000) {
        r.error_message = "dep_count excesivo (posible corrupcion)";
        return r;
    }
    if (dep_count > 0) {
        const uint64_t dep_table_end =
            static_cast<uint64_t>(dep_table_offset)
            + static_cast<uint64_t>(dep_count) * DEP_ENTRY_BYTES;
        if (dep_table_offset < HEADER_BYTES || dep_table_end > size) {
            r.error_message = "dep_table_offset fuera del fichero";
            return r;
        }
    }

    for (uint32_t i = 0; i < symbol_count; ++i) {
        VexiSymbol s;
        size_t entry_off = HEADER_BYTES + i * SYMENTRY_BYTES;
        uint8_t kind = 0, flags = 0;
        uint16_t align_override = 0;
        uint32_t name_off = 0, name_len = 0;
        uint32_t payload_off = 0, payload_len = 0;
        read_u8(data, size, entry_off, kind);
        read_u8(data, size, entry_off, flags);
        read_u16(data, size, entry_off, align_override);
        read_u32(data, size, entry_off, name_off);
        read_u32(data, size, entry_off, name_len);
        read_u32(data, size, entry_off, payload_off);
        read_u32(data, size, entry_off, payload_len);

        s.kind           = static_cast<VexiSymbolKind>(kind);
        s.is_opaque      = (flags & 0x01) != 0;
        s.is_extern      = (flags & 0x02) != 0;
        s.is_const       = (flags & 0x04) != 0;   // L.7
        s.align_override = align_override;
        if (!read_name(data, size, name_off, name_len, pool_start, s.name)) {
            r.error_message = "name fuera de bounds";
            return r;
        }

        bool ok = true;
        switch (s.kind) {
            case VexiSymbolKind::TYPEDEF_ALIAS:
            case VexiSymbolKind::TYPEDEF_NEW:
                ok = parse_payload_typedef(data, size, payload_off, payload_len,
                                             pool_start, s);
                break;
            case VexiSymbolKind::GLOBAL_VAR:
                ok = parse_payload_global(data, size, payload_off, payload_len,
                                            pool_start, s);
                break;
            case VexiSymbolKind::FUNCTION:
                ok = parse_payload_function(data, size, payload_off, payload_len,
                                              pool_start, s);
                break;
            case VexiSymbolKind::STRUCT:
            case VexiSymbolKind::CLASS:
                ok = parse_payload_struct_or_class(data, size, payload_off,
                                                     payload_len, pool_start, s);
                break;
            case VexiSymbolKind::ENUM:
                ok = parse_payload_enum(data, size, payload_off, payload_len,
                                          pool_start, s);
                break;
            default:
                ok = false;
                r.error_message = "kind de simbolo desconocido";
                break;
        }
        if (!ok) {
            if (r.error_message.empty()) {
                r.error_message = "fallo parseando payload del simbolo '" + s.name + "'";
            }
            return r;
        }
        r.module_.symbols.push_back(std::move(s));
    }

    // Phase M4.ext L.13: parsear dep table.
    r.module_.deps.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        size_t dep_off = dep_table_offset + i * DEP_ENTRY_BYTES;
        uint32_t n_off = 0, n_len = 0;
        uint32_t lo = 0, hi = 0;
        if (!read_u32(data, size, dep_off, n_off)) {
            r.error_message = "dep entry truncada";
            return r;
        }
        if (!read_u32(data, size, dep_off, n_len)) {
            r.error_message = "dep entry truncada";
            return r;
        }
        if (!read_u32(data, size, dep_off, lo)) {
            r.error_message = "dep entry truncada";
            return r;
        }
        if (!read_u32(data, size, dep_off, hi)) {
            r.error_message = "dep entry truncada";
            return r;
        }
        VexiModule::DepRecord d;
        d.abi_hash = static_cast<uint64_t>(lo)
                   | (static_cast<uint64_t>(hi) << 32);
        if (!read_name(data, size, n_off, n_len, pool_start, d.name)) {
            r.error_message = "dep name fuera de bounds";
            return r;
        }
        r.module_.deps.push_back(std::move(d));
    }

    r.ok = true;
    return r;
}

// Phase M5.b L.27: hash de la version del compilador.
//
// Combina:
//   - El @c VEXI_FORMAT_VERSION (cambios en el layout del .vexi).
//   - El timestamp de compilacion del compilador (__DATE__ + __TIME__).
//   - Una etiqueta de identidad ABI fija (incrementar manualmente al
//     cambiar reglas de mangling o de invariantes del checker).
//
// Si dos compiladores producen .vexi con el mismo hash, son
// ABI-compatibles a nivel cache.  Si difieren, el .vexi se descarta y
// regenera al siguiente compile.
uint64_t vexi_compiler_version_hash() noexcept {
    // String de identidad: includes una etiqueta ABI ('vex-1.0') + el
    // build time del compilador binario para detectar rebuilds del
    // compilador mismo.  El usuario puede sobreescribir definiendo
    // VEXI_COMPILER_BUILD_ID en CMake si quiere un hash determinista
    // independiente de __DATE__/__TIME__ (e.g. para CI reproducible).
    static constexpr const char *kIdentity =
#ifdef VEXI_COMPILER_BUILD_ID
        VEXI_COMPILER_BUILD_ID
#else
        "vex-1.0-abi" "|fmt:2|" __DATE__ "|" __TIME__
#endif
        ;
    static const uint64_t cached = vexi_fnv1a(kIdentity, std::strlen(kIdentity));
    return cached;
}

// ===========================================================================
// v4: helpers del blob_pool.
// ===========================================================================

uint32_t vexi_blob_append(std::vector<uint8_t> &pool,
                            VexiBlobKind kind,
                            const uint8_t *payload, size_t payload_len,
                            uint32_t element_size,
                            uint32_t count,
                            uint32_t alignment) {
    if (alignment < 8) alignment = 8;
    // Padding al multiplo de alignment.
    while (pool.size() % alignment != 0) pool.push_back(0);
    const uint32_t blob_off = static_cast<uint32_t>(pool.size());
    // Emitir BlobHeader (24 bytes, little-endian).
    write_u32(pool, static_cast<uint32_t>(kind));
    write_u32(pool, element_size);
    write_u32(pool, count);
    write_u32(pool, static_cast<uint32_t>(payload_len));
    const uint64_t h = vexi_fnv1a(payload, payload_len);
    write_u32(pool, static_cast<uint32_t>(h & 0xFFFFFFFFull));
    write_u32(pool, static_cast<uint32_t>((h >> 32) & 0xFFFFFFFFull));
    // Payload.
    pool.insert(pool.end(), payload, payload + payload_len);
    // Padding al multiplo de 8 para que el siguiente blob arranque alineado.
    while (pool.size() % 8 != 0) pool.push_back(0);
    return blob_off;
}

const VexiBlobHeader *vexi_blob_read(const std::vector<uint8_t> &pool,
                                       uint32_t offset) noexcept {
    if (offset + sizeof(VexiBlobHeader) > pool.size()) return nullptr;
    // Bytes little-endian -> volcamos a un objeto local con read manual.
    // Como VexiBlobHeader no tiene endianness fija en memoria, leemos via
    // un buffer estatico per-thread.  Para acceso rapido del consumer,
    // el TypeChecker materializa estos campos a una estructura propia.
    // Devolvemos un puntero "casteado" suponiendo que el host es LE
    // (la mayoria de targets actuales).  Para targets BE, anyadir
    // bytewise read y copia local.
    return reinterpret_cast<const VexiBlobHeader*>(pool.data() + offset);
}

const uint8_t *vexi_blob_payload(const std::vector<uint8_t> &pool,
                                   uint32_t offset) noexcept {
    if (offset + sizeof(VexiBlobHeader) > pool.size()) return nullptr;
    return pool.data() + offset + sizeof(VexiBlobHeader);
}

} // namespace vex
