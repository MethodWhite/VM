#pragma description VestaVM .velb executable format (v3)
#pragma magic [ 56 45 4C 42 ] @ 0x00

import std.mem;
import std.core;
import std.io;
import std.string;
import type.magic;

// ============================================================
//   VELB Executable Format - Compatible con ImHex 1.38.1+
//
//   Cubre las 3 versiones del header:
//     v1 = formato base (header + secciones + tablas + bytecode)
//     v2 = anyade reloc table  (para loadmodule con rebase transparente)
//     v3 = anyade IR section (@ir / VEIR) y debug section (DVBG)
//
//   El header tiene tamano fijo serializado de 144 bytes (alineado a 16).
//   El campo `address_spaces *` del struct C++ NO se serializa: la tabla
//   va inline justo despues del header.
// ============================================================

// Variables globales utiles para post-procesamiento
u64 g_bytecode_offset;
u64 g_bytecode_size;

// ============================================================
//   Tipos basicos
// ============================================================

struct velb_magic {
    char firma[4];                          // "VELB" en LE, "BLEV" en BE (raro)
} [[format("format_velb_magic")]];

fn format_velb_magic(velb_magic m) {
    return std::string::to_string(m.firma);
};

struct range_memory {
    u64 address_init;
    u64 address_final;
};

// Entrada de la tabla de espacios de direcciones (32 bytes packed).
// Estructura C++: address (16) + offset_section_strings (8) + offset_bytecode (8).
struct table_spaces_address {
    range_memory address;
    u64 offset_section_strings;
    u64 offset_bytecode;
};

// Entrada de la tabla de secciones (24 bytes packed).
// Estructura C++: address (16) + offset_string (8).
struct section_range_memory {
    range_memory address;
    char *name[] : u64;     // pointer-string: el u64 es offset; ImHex deref a cstring
};

// ============================================================
//   Header VELB (144 bytes fijos, alineado a 16)
// ============================================================

struct HeaderVELB {
    velb_magic magic;            //  +0  [4]   "VELB"
    u32 format_v;                //  +4  [4]   1, 2 o 3 (VERSION_VELB)

    u32 max_v;                   //  +8  [4]   max VM version compatible
    u32 min_v;                   // +12  [4]   min VM version compatible

    u64 checksum;                // +16  [8]   checksum del ejecutable
    u64 flags;                   // +24  [8]   meta-info (bits reservados, todos 0)
    u64 timestamp;               // +32  [8]   build timestamp (epoch)
    u32 arch;                    // +40  [4]   target_arch enum

    u32 count;                   // +44  [4]   numero de secciones en table
    u64 table_offset;            // +48  [8]   offset a tabla de secciones
    u64 n_spaces;                // +56  [8]   numero de espacios de direcciones
    u64 offset_section_strings;  // +64  [8]   offset a blob de strings de secciones
    u64 start_pc;                // +72  [8]   PC inicial (entry point)

    u64 offset_import_table;     // +80  [8]   tabla de importaciones FFI
    u64 offset_label_table;      // +88  [8]   tabla de labels (debug/symbols)

    u32 size_import_table;       // +96  [4]   entries en import_table
    u32 size_label_table;        // +100 [4]   entries en label_table

    // v3+: debug section (DVBG)
    u64 offset_debug_section;    // +104 [8]   0 = sin debug info
    u32 size_debug_section;      // +112 [4]   tamano total seccion DVBG
    u8  debug_level;             // +116 [1]   0=sin, 1=lineas, 2=+vars, 3=full
    u8  _debug_pad[3];           // +117 [3]   reservado (debe ser 0)

    // v2+: reloc table
    u64 offset_reloc_table;      // +120 [8]   0 = sin relocations
    u32 size_reloc_table;        // +128 [4]   numero de entries

    // v3+: IR section (VEIR)
    u64 offset_ir_section;       // +132 [8]   0 = sin IR (build sin JIT)
    u32 size_ir_section;         // +140 [4]   tamano total seccion @ir

    // Total: 144 bytes. Alineado a 16. Listo para la tabla de espacios.

    // Tabla de espacios de direcciones (n_spaces entries de 32 bytes cada una).
    // Va inline tras el header.
    table_spaces_address address_spaces[n_spaces];
};

// ============================================================
//   Tabla de secciones (entries de 24 bytes)
// ============================================================

// Se mappea a header.table_offset.
// Cada entry: address_init/final (16) + offset_string (8).

// ============================================================
//   Tabla de importacion FFI (entries de 16 bytes)
// ============================================================

struct entry_import_table {
    char *module_string[] : u32;     // offset a "kernel32.dll" etc.
    char *function_string[] : u32;   // offset a "GetTickCount" etc.
    u32 offset_signature_string;     // offset a "(i32)->i32" etc.
    u32 offset_bytecode;             // offset RELATIVO al bytecode de la instr CALLN a parchear
};

// ============================================================
//   Tabla de labels (entries de 16 bytes)
// ============================================================

struct entry_label_table {
    u32 offset_table_string;     // offset al nombre del label en strings blob
    u32 offset_bytecode;         // offset relativo al bytecode
    u32 index_section;           // indice de la seccion a la que pertenece
    u32 size_label;              // tamano del label en bytecode
};

// ============================================================
//   Tabla de relocations (v2+, entries de 24 bytes)
// ============================================================

enum RelocType : u8 {
    NONE        = 0,
    ABSOLUTE64  = 1,
    RELATIVE32  = 2,
    RELATIVE64  = 3,
    ABSOLUTE32  = 4,    // truncado a 32 bits (cmpjmp, decjnz)
};

struct entry_relocation_table {
    u64 bytecode_offset;     // offset desde inicio de bytes ejecutables
    u64 target_value;        // VA original del label
    RelocType type;          // 1 byte
    u8  _pad[7];             // relleno hasta 24 bytes
};

// ============================================================
//   Seccion debug (v3+, magic DVBG)
//
//   Layout:
//     [DebugSectionHeader (24 bytes)]
//     [DebugLineEntry[]   (12 bytes c/u, level >= 1)]
//     [DebugVarEntry[]    (level >= 2; deferido)]
//     [DebugScopeEntry[]  (level == 3; deferido)]
//     [strings blob de tamano strings_size]
// ============================================================

struct DebugSectionHeader {
    char magic[4];           // "DVBG" (0x47425644 en LE)
    u8   version;            // formato (actualmente 1)
    u8   level;              // 1, 2 o 3
    u16  _pad;               // relleno (debe ser 0)
    u32  line_count;
    u32  var_count;          // 0 si level < 2
    u32  scope_count;        // 0 si level < 3
    u32  strings_size;       // bytes del blob de strings al final
};

struct DebugLineEntry {
    u32 bytecode_offset;     // offset en el bytecode
    u32 line_number;         // linea en archivo fuente (base 1)
    u32 file_offset;         // offset al nombre del archivo en strings blob
};

struct DebugSection {
    DebugSectionHeader hdr;
    DebugLineEntry lines[hdr.line_count];
    // (DebugVarEntry[] y DebugScopeEntry[] omitidos si level == 1)
    u8 strings_blob[hdr.strings_size];
};

// ============================================================
//   Seccion @ir (v3+, magic VEIR)
//
//   Layout:
//     +0  [4]  magic "VEIR" (0x52494556 en LE)
//     +4  [2]  version u16
//     +6  [2]  reserved u16
//     +8  [4]  function_count
//     +12 [..] IrFunctions serializadas (back-to-back, formato variable)
//
//   El cuerpo de las IrFunctions usa un encoding propio descrito en
//   include/ir/ssa_ir_serialize.h.  Se muestra como blob de bytes; un
//   parser completo requeriria reimplementar el deserializer.
// ============================================================

struct IRSectionHeader {
    char magic[4];           // "VEIR"
    u16  version;
    u16  reserved;
    u32  function_count;
};

struct IRSection<auto Size> {
    IRSectionHeader hdr;
    u8 body[Size - sizeof(IRSectionHeader)];
};

// ============================================================
//   Tabla de relocations envoltorio
// ============================================================

struct RelocTable<auto Count> {
    entry_relocation_table entries[Count];
};

// ============================================================
//   Bytecode (region entre secciones y primera tabla auxiliar)
// ============================================================

// Heuristica: el bytecode empieza tras la tabla de secciones y termina
// donde empieza la primera tabla auxiliar presente (import / debug /
// reloc / ir / EOF lo que llegue primero).

fn min_present(u64 a, u64 b) {
    if (a == 0) return b;
    if (b == 0) return a;
    return (a < b) ? a : b;
};

struct Bytecode {
    u8 data[g_bytecode_size];
};

// ============================================================
//   Aplicacion del pattern al archivo
// ============================================================

HeaderVELB header @ 0x00;

// Calcular fronteras del bytecode
u64 sections_end = header.table_offset + header.count * 24;
u64 file_size    = std::mem::size();

u64 first_tail = min_present(header.offset_import_table,
                  min_present(header.offset_label_table,
                  min_present(header.offset_debug_section,
                  min_present(header.offset_reloc_table,
                              header.offset_ir_section))));
if (first_tail == 0) {
    first_tail = file_size;
}

g_bytecode_offset = sections_end;
g_bytecode_size   = (first_tail > sections_end) ? (first_tail - sections_end) : 0;

// --- Tabla de secciones ---
section_range_memory sections[header.count] @ header.table_offset;

// --- Bytecode ---
Bytecode bytecode @ g_bytecode_offset;

// --- Tabla de importaciones (FFI) ---
if (header.size_import_table > 0 && header.offset_import_table > 0) {
    entry_import_table import_table[header.size_import_table] @ header.offset_import_table;
}

// --- Tabla de labels ---
if (header.size_label_table > 0 && header.offset_label_table > 0) {
    entry_label_table label_table[header.size_label_table] @ header.offset_label_table;
}

// --- Tabla de relocations (v2+) ---
if (header.size_reloc_table > 0 && header.offset_reloc_table > 0) {
    RelocTable<header.size_reloc_table> reloc_table @ header.offset_reloc_table;
}

// --- Seccion @ir (v3+) ---
if (header.size_ir_section > 0 && header.offset_ir_section > 0) {
    IRSection<header.size_ir_section> ir_section @ header.offset_ir_section;
}

// --- Seccion debug DVBG (v3+) ---
if (header.size_debug_section > 0 && header.offset_debug_section > 0) {
    DebugSection debug_section @ header.offset_debug_section;
}

// ============================================================
//   Reporte por consola
// ============================================================

std::print("=== VELB v{} ===", header.format_v);
std::print("  Bytecode: offset=0x{:X} size={} bytes",
           g_bytecode_offset, g_bytecode_size);
std::print("  Secciones: count={} @ offset 0x{:X}",
           header.count, header.table_offset);
std::print("  Import table: {} entries @ offset 0x{:X}",
           header.size_import_table, header.offset_import_table);
std::print("  Label table: {} entries @ offset 0x{:X}",
           header.size_label_table, header.offset_label_table);
if (header.size_reloc_table > 0) {
    std::print("  Reloc table: {} entries @ offset 0x{:X}",
               header.size_reloc_table, header.offset_reloc_table);
} else {
    std::print("  Reloc table: (ninguna)");
}
if (header.size_ir_section > 0) {
    std::print("  IR section (@ir): {} bytes @ offset 0x{:X}",
               header.size_ir_section, header.offset_ir_section);
} else {
    std::print("  IR section (@ir): (ninguna, build sin JIT)");
}
if (header.size_debug_section > 0) {
    std::print("  Debug section (DVBG): {} bytes @ offset 0x{:X} (level {})",
               header.size_debug_section, header.offset_debug_section,
               header.debug_level);
} else {
    std::print("  Debug section (DVBG): (ninguna)");
}
std::print("  Total file size: {} bytes", file_size);
