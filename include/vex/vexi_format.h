/**
 * @file vexi_format.h
 * @brief Formato binario del fichero de interfaz `.vexi` (Phase M.2).
 *
 * El `.vexi` es la interfaz publica de un modulo Vex.  Contiene SOLO los
 * simbolos exportados (marcados `public`) + sus firmas, sin
 * implementacion.  El TypeChecker de un modulo CONSUMIDOR carga los
 * `.vexi` de sus deps para resolver nombres cross-module sin reparsear
 * el source de cada dep.
 *
 * Caracteristicas hardware-aware:
 *   - Layout flat little-endian (cache-friendly, sin pointer chasing).
 *   - String pool al final (los nombres referencian offsets dentro del
 *     pool).  Permite hashing rapido O(1) por (offset+len) sin copiar.
 *   - Hash ABI FNV-1a 64 cubre TODOS los simbolos publicos en orden;
 *     dos modulos compilados con el mismo source + mismas deps producen
 *     el mismo ABI hash (detectable por el linker).
 *   - Reserva anticipada del buffer de salida en el emitter.
 *
 * El parser es zero-copy donde posible: los nombres se materializan como
 * `string_view` al pool sin alocar nuevos `std::string` hasta que el
 * caller lo necesite.  Para fields/methods se construyen colecciones
 * propias del TypeChecker.
 *
 * Multi-platform: bytes little-endian explicitos (no usa __PRAGMA pack
 * ni layout dependiente del compilador).  Compila identico en GCC,
 * Clang, MSVC, x86-64, ARM64, RISC-V.
 */

#ifndef VEX_VEXI_FORMAT_H
#define VEX_VEXI_FORMAT_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

/// Magic del fichero `.vexi` (4 bytes ASCII "VEXI").
inline constexpr uint32_t VEXI_MAGIC = 0x49584556u;

/// Version del formato.  Incrementar al cambiar el layout binario.  El
/// loader rechaza versiones distintas (no se intenta forward-compat).
/// v2 (Phase M5.b): anyade `extern_lib_len` explicito en el payload
/// FUNCTION (cierra L.5).
/// v3 (M4.ext): dep_table para invalidacion transitiva del cache.
/// v4 (.vexi-v4): **blob_pool** -- buffer contiguo para valores no
/// escalares (string, array, struct) de comptime const cross-module.
/// El Symbol GLOBAL_VAR ahora puede llevar @c blob_offset apuntando al
/// pool en lugar de @c init_value (que solo cabe 8 bytes).  Atributos
/// `@align`/`@hot`/`@cold`/`@section` viajan en el blob.
inline constexpr uint16_t VEXI_FORMAT_VERSION = 5;

/// Kind del payload dentro de un BlobHeader (.vexi v4).  Asignaciones
/// estables (persisten en disco).  Cualquier kind desconocido = saltar.
enum class VexiBlobKind : uint32_t {
    NONE         = 0,   ///< sentinel; no aplica.
    STRING       = 1,   ///< bytes UTF-8 sin NUL terminator.
    ARRAY_PRIM   = 2,   ///< array de primitivos (i8..u64, bool, char).
    STRUCT_PRIM  = 3,   ///< struct con campos primitivos.
    // Reservados v5+:
    STRUCT_NESTED= 4,   ///< struct con campos compuestos (referencias a otros blobs).
    ARRAY_REF    = 5,   ///< array de referencias (e.g. array de strings).
    TYPE_DESC    = 6,   ///< type-as-value descriptor.
};

/// BlobHeader: cabecera uniforme (24 bytes) de cada blob dentro del pool.
/// Permite que herramientas (desensamblador, dumper, port a C) recorran
/// el pool sin conocer las kinds futuras: para un kind desconocido,
/// saltan @c total_bytes.  Reads alineados a 8 bytes.
struct VexiBlobHeader {
    uint32_t kind = 0;          ///< VexiBlobKind
    uint32_t element_size = 0;  ///< STRING:1; ARRAY_PRIM:sizeof(elem); STRUCT:0
    uint32_t count = 0;          ///< STRING:byte_len; ARRAY_PRIM:N; STRUCT:field_count
    uint32_t total_bytes = 0;    ///< payload bytes que siguen (sin contar header)
    uint64_t content_hash = 0;   ///< FNV-1a 64 sobre payload; permite dedup
    // payload[total_bytes] aligned a 8.
};

/// Categorias de simbolos exportados.  Asignaciones estables (no
/// reorderear; el byte se persiste en disco).
enum class VexiSymbolKind : uint8_t {
    TYPEDEF_ALIAS  = 1,   ///< typedef transparente (T = u32).
    TYPEDEF_NEW    = 2,   ///< newtype (typedef T name new [@opaque] [@align(N)]).
    STRUCT         = 3,   ///< struct value-type con su layout completo.
    CLASS          = 4,   ///< class (incluye superclase, fields, metodos).
    ENUM           = 5,   ///< enum (ADT) con variantes + payloads.
    GLOBAL_VAR     = 6,   ///< global variable (typed).
    FUNCTION       = 7,   ///< funcion top-level con firma.
};

/// Cabecera del fichero .vexi (48 bytes en v3).
///
/// Layout binario (offsets little-endian):
/// @verbatim
/// +0   u32  magic = 'VEXI' (0x49584556)
/// +4   u16  format_version
/// +6   u16  _reserved
/// +8   u64  abi_hash      = FNV-1a 64 sobre simbolos publicos en orden
/// +16  u64  source_hash   = FNV-1a 64 sobre el .vex source crudo
/// +24  u64  compiler_version_hash  (M5.b L.27)
/// +32  u32  symbol_count
/// +36  u32  string_pool_offset (relativo al inicio del fichero)
/// +40  u32  dep_count   (M4.ext L.13: deps directos)
/// +44  u32  dep_table_offset  (relativo al inicio del fichero)
/// +48  ... entries (variable length) ...
/// +    ... dep table (16 bytes c/u: u64 name_off+name_len packed
///                                 + u64 abi_hash) ...
/// +    ... string pool al final ...
/// @endverbatim
struct VexiHeader {
    uint32_t magic = VEXI_MAGIC;
    uint16_t format_version = VEXI_FORMAT_VERSION;
    uint16_t _reserved = 0;
    uint64_t abi_hash = 0;
    uint64_t source_hash = 0;
    /// Phase M5.b L.27: hash de la version del compilador.  Si el .vexi
    /// fue generado por una version anterior cuyas reglas de mangling
    /// o layout difieren, el loader debe rechazar el cache y regenerar.
    /// El valor lo provee el compilador via @c vexi_compiler_version_hash().
    uint64_t compiler_version_hash = 0;
    uint32_t symbol_count = 0;
    uint32_t string_pool_offset = 0;
    /// Phase M4.ext L.13: cache transitivo.  El .vexi guarda los
    /// (module_name, abi_hash) de cada dep directo del modulo.  Al
    /// cache hit, el loader verifica que TODOS los deps siguen con el
    /// mismo abi_hash que cuando se compilo este modulo; si alguno
    /// cambio, invalida el cache y fuerza recompilar.
    uint32_t dep_count = 0;
    uint32_t dep_table_offset = 0;
    /// v4: blob_pool.  Buffer contiguo de bytes para los valores de
    /// `comptime` no escalares (string, array, struct).  Cada simbolo
    /// que use el pool lleva @c blob_offset apuntando a un @c BlobHeader
    /// dentro de este buffer.  Los blobs se alinean a
    /// @c blob_pool_alignment bytes para permitir reads alineados.
    uint32_t blob_pool_offset = 0;
    uint32_t blob_pool_size = 0;
    uint8_t  blob_pool_alignment = 8;  ///< default 8; reservado para AVX
    uint8_t  _pad[7] = {0,0,0,0,0,0,0};
};

/**
 * @brief Descriptor in-memory de un simbolo exportado en el .vexi.
 *
 * Representacion alta-nivel decodificada del binario.  El parser materializa
 * estos struct para que el TypeChecker los consuma sin tener que tocar
 * los bytes raw.
 *
 * El layout binario per-kind se documenta en la implementacion del
 * emitter (`vexi_format.cpp`).
 */
struct VexiSymbol {
    VexiSymbolKind kind = VexiSymbolKind::FUNCTION;
    std::string    name;                 ///< Nombre publico del simbolo.

    // Comun: el payload depende de @c kind.  Solo los campos relevantes
    // se llenan; el resto queda con su valor default.

    // === TYPEDEF_ALIAS / TYPEDEF_NEW ===
    /// Tipo subyacente en forma textual canonica (e.g. "u64", "i32*").
    /// El TypeChecker re-resuelve el TypeNode parseando este string.
    std::string    underlying_type;
    /// (TYPEDEF_NEW) Bits ocultos al cliente (`@opaque`).
    bool           is_opaque = false;
    /// (GLOBAL_VAR) marca `const` propagada al consumidor.  L.7.
    bool           is_const = false;
    /// (GLOBAL_VAR) marca @c true si el .vexi contiene el valor literal
    /// inicial (solo aplicable a const numericas).  L.7.
    bool           has_init_value = false;
    /// (GLOBAL_VAR) Valor inicial entero (raw u64, signo segun el tipo).
    /// Solo significativo si @c has_init_value es @c true.
    uint64_t       init_value = 0;
    /// v4: si @c has_blob_ref es @c true, el valor inicial vive en el
    /// blob_pool del .vexi en @c blob_offset.  Este campo es excluyente
    /// con @c has_init_value (un simbolo es escalar inline O via blob).
    /// El consumer materializa el blob al @c static_data del modulo en
    /// el primer uso.
    bool           has_blob_ref = false;
    uint32_t       blob_offset = 0;
    /// v4: hint del kind del blob.  Copia del @c BlobHeader::kind para
    /// que el consumer pueda decidir sin chase del puntero.  0 = NONE.
    uint8_t        blob_kind_hint = 0;
    /// v4: atributos de usuario (`@align`, `@hot`, `@cold`, `@section`).
    /// Solo se persisten para GLOBAL_VAR con blob ref; los atributos en
    /// runtime const numericas se aplican al lowering local del consumer.
    /// Bits: 0=hot, 1=cold; los demas reservados.  Alineacion en bytes.
    uint8_t        attr_flags = 0;
    uint16_t       attr_align = 0;       ///< 0 = default; sino multiplo de 8/16/32/64
    std::string    attr_section;          ///< vacio = default; sino nombre de seccion
    /// (TYPEDEF_NEW) Alineacion forzada (`@align(N)`).  0 = default.
    uint16_t       align_override = 0;
    /// (TYPEDEF_NEW) IDs nominales se asignan localmente al re-importar.
    /// El que se persiste aqui es el ABI hash (FNV-1a del nombre).
    uint64_t       nominal_abi = 0;

    /// Phase M.L8: bloque @c {explicit from/to T;} del typedef new.
    /// Cada entry es @c (typename_canonico, is_public).  El consumidor
    /// solo puede usar conversiones marcadas @c is_public.
    struct ExplicitConvEntry {
        std::string type_str;
        bool        is_public = false;
    };
    std::vector<ExplicitConvEntry> from_conversions;
    std::vector<ExplicitConvEntry> to_conversions;

    // === STRUCT / CLASS ===
    struct FieldInfo {
        std::string name;
        std::string type_str;        ///< typename canonico
        uint32_t    offset = 0;
        uint32_t    size = 0;
        uint8_t     bit_offset = 0;
        uint8_t     bit_width = 0;
    };
    std::vector<FieldInfo> fields;
    /// (CLASS) Nombre de la superclase o vacio.
    std::string            super_class;
    /// (CLASS) Interfaces implementadas (nombres).
    std::vector<std::string> interfaces;
    /// (CLASS) Phase M6.b: firmas de metodos publicos para que el
    /// consumidor pueda emitir CALLVIRT correctamente cross-module.
    /// Cierra L.6.
    struct MethodInfo {
        std::string              name;          ///< nombre publico ("area")
        std::string              return_type;   ///< typename canonico
        std::vector<std::string> param_types;   ///< typenames canonicos
        uint32_t                 vtable_index = 0;
        uint8_t                  flags = 0;     ///< bit0=is_static, bit1=is_constructor, bit2=is_virtual
        std::string              mangled_label; ///< label real en .vel para CALLVM directo si !is_virtual
    };
    std::vector<MethodInfo> methods;
    /// (STRUCT / CLASS) Tamano total + alineacion.
    uint32_t       size_bytes = 0;
    uint32_t       align_bytes = 1;

    // === ENUM ===
    struct EnumVariant {
        std::string              name;
        uint32_t                 tag = 0;
        std::vector<std::string> payload_types;
    };
    std::vector<EnumVariant> variants;

    // === FUNCTION ===
    std::string              return_type;     ///< typename canonico del retorno
    std::vector<std::string> param_types;     ///< typenames canonicos en orden
    std::vector<std::string> param_names;     ///< paralelo a param_types
    bool                     is_extern = false;     ///< extern "lib.dll"
    std::string              extern_lib;            ///< si is_extern
    /// Phase M.5: label internal usado en el .vel del modulo origen.
    /// Vacio = mismo que @c name.  Si distinto, el consumidor emite
    /// @c CALLVM al @c mangled_label en lugar de @c name (resuelve
    /// colisiones de nombres cross-module).  E.g. funcion "sumar" del
    /// modulo "lib" tiene @c name="sumar" pero @c mangled_label="lib__sumar".
    std::string              mangled_label;
};

/**
 * @brief Modulo de interfaz decodificado.
 */
struct VexiModule {
    uint16_t                 format_version       = 0;
    uint64_t                 abi_hash             = 0;
    uint64_t                 source_hash          = 0;
    uint64_t                 compiler_version_hash = 0;   ///< M5.b L.27
    std::vector<VexiSymbol>  symbols;
    /// Phase M4.ext L.13: cache transitivo.  Cada DepRecord guarda el
    /// nombre del modulo dep + su abi_hash en el momento de compilar
    /// este .vexi.  El loader verifica los deps al cache hit.
    struct DepRecord {
        std::string name;
        uint64_t    abi_hash = 0;
    };
    std::vector<DepRecord>   deps;
    /// v4: blob_pool.  Bytes contiguos para los valores de simbolos
    /// con @c VexiSymbol::has_blob_ref=true.  Cada blob arranca en un
    /// multiplo de 8 (`blob_pool_alignment`) y tiene un @c VexiBlobHeader
    /// al inicio.  Vacio si ningun simbolo necesita blobs.
    std::vector<uint8_t>     blob_pool;
    uint8_t                  blob_pool_alignment = 8;
};

/// Helper: alocar un blob en el pool y devolver su offset.  El emitter
/// lo usa al construir el VexiModule antes de serializar.  Padding
/// automatico al multiplo de @c alignment.
///
/// @param pool       buffer destino (in-out)
/// @param kind       VexiBlobKind del blob a alocar
/// @param payload    bytes del payload (alineados internamente al elem_size)
/// @param element_size 1/2/4/8 para arrays primitivos; 1 para strings
/// @param count      elementos (o byte_len para strings)
/// @param alignment  alineamiento del header (default 8)
/// @return offset del header dentro del pool
uint32_t vexi_blob_append(std::vector<uint8_t> &pool,
                            VexiBlobKind kind,
                            const uint8_t *payload, size_t payload_len,
                            uint32_t element_size,
                            uint32_t count,
                            uint32_t alignment = 8);

/// Helper: leer un BlobHeader del pool en @c offset.  Devuelve nullptr
/// si @c offset esta fuera de rango o el header esta truncado.  No
/// valida la integridad del payload (caller debe respetar @c total_bytes).
const VexiBlobHeader *vexi_blob_read(const std::vector<uint8_t> &pool,
                                       uint32_t offset) noexcept;

/// Helper: devuelve puntero al payload del blob en @c offset (justo
/// despues del header).  Caller asume responsabilidad de respetar
/// @c total_bytes para no leer fuera de rango.
const uint8_t *vexi_blob_payload(const std::vector<uint8_t> &pool,
                                   uint32_t offset) noexcept;

/**
 * @brief Hash constante del compilador en uso.  Definido por una macro
 * @c VEXI_COMPILER_BUILD_ID (sobreescribible en CMake) o por defecto al
 * FNV-1a del string @c "vex-1.0.0" / fecha de build.  Si dos compiladores
 * tienen el mismo hash, sus .vexi son ABI-compatibles.  Si difieren, el
 * cache se invalida.
 */
uint64_t vexi_compiler_version_hash() noexcept;

/**
 * @brief Serializa un @c VexiModule a bytes listos para escribir a disco.
 *
 * @param mod Modulo con simbolos publicos a exportar.
 * @return Vector de bytes con el header + entries + string pool.  El
 *         abi_hash se RECALCULA durante la emision sobre los simbolos
 *         en orden.
 *
 * Optimizaciones:
 *   - Reserva del buffer estimada con base en el conteo de simbolos.
 *   - String pool con dedup (mismo nombre = un solo offset).
 *   - FNV-1a streaming durante el emit (no requiere segunda pasada).
 */
std::vector<uint8_t> vexi_emit(const VexiModule &mod);

/**
 * @brief Resultado del parsing de un .vexi.
 */
struct VexiParseResult {
    bool        ok = false;
    std::string error_message;
    VexiModule  module_;
};

/**
 * @brief Parsea bytes de un .vexi y devuelve un @c VexiModule decodificado.
 *
 * Validaciones:
 *   - magic == VEXI_MAGIC
 *   - format_version == VEXI_FORMAT_VERSION (rechaza otras)
 *   - offsets dentro del buffer (no OOB reads)
 *   - string_pool_offset coherente
 *
 * Si falla devuelve @c ok=false + mensaje detallado.
 */
VexiParseResult vexi_parse(const uint8_t *data, size_t size);

/**
 * @brief FNV-1a 64-bit publico (mismo helper que ModuleGraph).
 *
 * Expuesto aqui para que el TypeChecker pueda calcular el source_hash
 * sin importar el resolver.
 */
uint64_t vexi_fnv1a(const void *data, size_t len) noexcept;

inline uint64_t vexi_fnv1a(const std::string &s) noexcept {
    return vexi_fnv1a(s.data(), s.size());
}

} // namespace vex

#endif // VEX_VEXI_FORMAT_H
