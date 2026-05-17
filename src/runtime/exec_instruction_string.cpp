/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file exec_instruction_string.cpp
 * @brief Implementacion del sistema de cadenas de texto de VestaVM.
 *
 * == Instrucciones implementadas ==
 *
 *   0x46  STRMAKE  r_dst, r_src, r_len, enc
 *         Crea un StringObject FLAT desde un buffer en memoria VM.
 *         Aplica compactacion automatica de codificacion (HotSpot-style).
 *         Interna automaticamente strings <= STR_INTERN_THRESHOLD bytes.
 *
 *   0x47  STRLEN   r_dst, r_src
 *         Devuelve el numero de code points.  Funciona para FLAT/ROPE/SLICE.
 *
 *   0x48  STRCAT   r_dst, r_a, r_b
 *         Crea un nodo ROPE (concatenacion perezosa O(1)) si ambos operandos
 *         tienen contenido; devuelve el otro si uno es vacio.
 *
 *   0x49  STRCMP   r_dst, r_a, r_b
 *         Comparacion lexicografica.  Materializa ROPE/SLICE si es necesario.
 *
 *   0x4A  STRCONV  r_dst, r_src, enc
 *         Convierte la codificacion.  Implementa UTF-8 <-> UTF-16LE completo.
 *
 *   0x4B  STRRAW   r_dst, r_src
 *         Devuelve puntero host al buffer.  Materializa ROPE/SLICE primero.
 *
 *   0x4C  STRSLICE r_dst, r_src, r_start, r_len
 *         Crea un nodo SLICE (vista sin copia O(1)).  Materializa si el padre
 *         es ROPE.  r_start y r_len son en code points.
 *
 *   0x4D  STRFLAT  r_dst, r_src
 *         Materializa cualquier ROPE o SLICE a FLAT.  Operacion identidad
 *         si el origen ya es FLAT.  Internado automatico tras materializacion.
 *
 *   0x4E  STRHASH  r_dst, r_src
 *         Devuelve el hash FNV-1a (calcula y cachea si es necesario).
 *         Materializa ROPE/SLICE antes de calcular.
 *
 *   0x4F  STRINTERN r_dst, r_src
 *         Interna el string en el pool del proceso.  Devuelve el GcHandle
 *         canonico (puede ser distinto de r_src si ya habia un identico).
 *
 *   0x50  STRGETENC  r_dst, r_src   -> encoding byte (0-4)
 *   0x51  STRGETBYTES r_dst, r_src  -> byte_len
 *   0x52  STRGETKIND  r_dst, r_src  -> kind (0=FLAT 1=ROPE 2=SLICE)
 *   0x53  STRRESERVE  r_dst, r_cap  -> GcHandle de FLAT con capacidad reservada
 *         Crea un FLAT vacio con byte_len=0 pero capacidad interna = r_cap bytes.
 *         El programa escribe en el buffer via STRRAW + instrucciones nativas.
 *         Tras escribir, usa STRFINALIZE para establecer la longitud final.
 *   0x54  STRFINALIZE r_dst, r_newlen -> actualiza byte_len+length+hash de un FLAT mutable
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"           // para acceder a vm->scheduler.vm_reference.script_args
#include "runtime/scheduler.h"
#include "runtime/string_intern.h"
#include "gc/gc_heap.h"
#include "loader/oop_types.h"
#include "loader/string_object.h"

#include <cstring>   // memcpy, memcmp
#include <vector>
#include <string>
#include <algorithm> // std::min

namespace runtime {

// =========================================================================
// Forward declarations internas
// =========================================================================

static gc::GcHandle flatten_string(ProcessVM *vm, gc::GcHandle h);
static gc::GcHandle auto_intern(ProcessVM *vm, gc::GcHandle h, const uint8_t *data, uint32_t byte_len, loader::StringEncoding enc);

// =========================================================================
// Helper: obtener o crear el pool de interning del proceso
// =========================================================================

/**
 * @brief Devuelve el pool de interning del proceso, creandolo si no existe.
 *
 * El pool se crea de forma lazy la primera vez que se necesita.
 *
 * @param vm  Proceso virtual propietario del pool.
 * @return    Referencia al StringInternPool del proceso.
 */
static StringInternPool &get_intern_pool(ProcessVM *vm) {
    if (!vm->str_intern_pool)
        vm->str_intern_pool = new StringInternPool(); // creacion lazy del pool
    return *vm->str_intern_pool;
}

// =========================================================================
// Helper: construir clave de interning
// =========================================================================

/**
 * @brief Construye la clave de busqueda del pool a partir de bytes + encoding.
 *
 * @param data      Buffer de bytes del string.
 * @param byte_len  Numero de bytes.
 * @param enc       Codificacion del string.
 * @return          Clave std::string (bytes del string + byte de encoding).
 */
static std::string make_intern_key(const uint8_t *data, uint32_t byte_len, loader::StringEncoding enc) {
    std::string key(reinterpret_cast<const char *>(data), byte_len); // raw bytes
    key += static_cast<char>(static_cast<uint8_t>(enc));             // byte de codificacion al final
    return key;
}

// =========================================================================
// Helper: alocar y construir un StringObject FLAT en el GcHeap
// =========================================================================

/**
 * @brief Aloca un StringObject FLAT en el GcHeap con los bytes dados.
 *
 * Asigna sizeof(StringObject) + byte_len + 1 bytes.
 * El byte extra al final garantiza la terminacion nula para Win32 FFI.
 *
 * @param vm        Proceso virtual propietario del heap.
 * @param src_data  Buffer de entrada (puede ser nullptr para string vacio).
 * @param byte_len  Numero de bytes del contenido (sin el nulo extra).
 * @param length    Numero de code points.
 * @param enc       Codificacion del string.
 * @param capacity  Si > byte_len, reserva espacio adicional (para STRRESERVE).
 * @return          GcHandle del nuevo StringObject o GC_NULL_HANDLE.
 */
static gc::GcHandle alloc_flat(ProcessVM *vm,
                                const uint8_t *src_data,
                                uint32_t byte_len,
                                uint32_t length,
                                loader::StringEncoding enc,
                                uint32_t capacity = 0)
{
    uint32_t buf_size = (capacity > byte_len) ? capacity : byte_len; // usar el mayor
    size_t total = sizeof(loader::StringObject) + buf_size + 1;      // +1 para nulo Win32

    // alloc_pinned: aloca directo en OldGen (non-moving).  Necesario porque
    // STRRAW exporta el host_ptr al buffer y este se preserva via push/pop a
    // traves de calls que pueden disparar GC.  Si el StringObject estuviera
    // en young y se evacuara, el host_ptr quedaria dangling.
    gc::GcHandle h = vm->gc_heap.alloc_pinned(total);
    if (h == gc::GC_NULL_HANDLE) return gc::GC_NULL_HANDLE;

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return gc::GC_NULL_HANDLE;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);

    // inicializar cabecera ObjectHeader
    s->header.class_ptr  = nullptr;
    s->header.flags      = loader::OBJ_FLAG_GC_OWNED;
    s->header.hash_code  = static_cast<uint32_t>(h);
    s->header.owner_pid  = 0;
    s->header.lock_depth = 0;
    s->header._mon_pad   = 0;

    s->encoding = static_cast<uint8_t>(enc);
    s->kind     = static_cast<uint8_t>(loader::StringKind::FLAT);
    s->_pad[0]  = s->_pad[1] = 0;
    s->length   = length;
    s->byte_len = byte_len; // byte_len es el contenido logico, no la capacidad total
    s->str_hash = 0;        // hash calculado bajo demanda

    uint8_t *dst = loader::str_data(s);
    if (src_data && byte_len > 0) std::memcpy(dst, src_data, byte_len);
    dst[byte_len] = 0; // terminador nulo siempre presente

    // calcular hash inmediatamente para strings cortos (optimizacion de rendimiento)
    if (byte_len > 0 && byte_len <= loader::STR_INTERN_THRESHOLD)
        loader::str_hash_compute(s);

    return h;
}

/**
 * @brief Aloca un nodo ROPE en el GcHeap.
 *
 * El rope no tiene buffer de datos; almacena RopeData (left/right handles).
 * La concatenacion es perezosa: no se copia ningun byte.
 *
 * @param vm     Proceso virtual.
 * @param left   GcHandle del string izquierdo.
 * @param right  GcHandle del string derecho.
 * @param enc    Codificacion del rope (hereda del hijo izquierdo).
 * @return       GcHandle del nuevo nodo ROPE.
 */
static gc::GcHandle alloc_rope(ProcessVM *vm,
                                gc::GcHandle left,
                                gc::GcHandle right,
                                uint32_t total_length,
                                uint32_t total_byte_len,
                                loader::StringEncoding enc,
                                uint32_t depth)
{
    size_t total = sizeof(loader::StringObject) + sizeof(loader::RopeData);
    // Vease nota en alloc_flat: alloc_pinned -> OldGen non-moving para que el
    // host_ptr exportado via STRRAW no quede dangling tras un GC menor.
    gc::GcHandle h = vm->gc_heap.alloc_pinned(total);
    if (h == gc::GC_NULL_HANDLE) return gc::GC_NULL_HANDLE;

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return gc::GC_NULL_HANDLE;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);

    s->header.class_ptr  = nullptr;
    s->header.flags      = loader::OBJ_FLAG_GC_OWNED;
    s->header.hash_code  = static_cast<uint32_t>(h);
    s->header.owner_pid  = 0;
    s->header.lock_depth = 0;
    s->header._mon_pad   = 0;

    s->encoding = static_cast<uint8_t>(enc);
    s->kind     = static_cast<uint8_t>(loader::StringKind::ROPE);
    s->_pad[0]  = s->_pad[1] = 0;
    s->length   = total_length;
    s->byte_len = total_byte_len;
    s->str_hash = 0;

    auto *rd = loader::str_rope(s);
    rd->left_handle  = left;
    rd->right_handle = right;
    rd->depth        = depth;
    rd->_pad         = 0;

    return h;
}

/**
 * @brief Aloca un nodo SLICE en el GcHeap.
 *
 * El slice es una vista zero-copy sobre un segmento de un StringObject FLAT.
 *
 * @param vm          Proceso virtual.
 * @param parent      GcHandle del StringObject FLAT padre.
 * @param byte_offset Offset en bytes dentro del buffer del padre.
 * @param byte_len    Numero de bytes del slice.
 * @param length      Numero de code points del slice.
 * @param enc         Codificacion (heredada del padre).
 * @return            GcHandle del nuevo nodo SLICE.
 */
static gc::GcHandle alloc_slice(ProcessVM *vm,
                                 gc::GcHandle parent,
                                 uint32_t byte_offset,
                                 uint32_t byte_len,
                                 uint32_t length,
                                 loader::StringEncoding enc)
{
    size_t total = sizeof(loader::StringObject) + sizeof(loader::SliceData);
    // Vease nota en alloc_flat: alloc_pinned -> OldGen non-moving.
    gc::GcHandle h = vm->gc_heap.alloc_pinned(total);
    if (h == gc::GC_NULL_HANDLE) return gc::GC_NULL_HANDLE;

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return gc::GC_NULL_HANDLE;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);

    s->header.class_ptr  = nullptr;
    s->header.flags      = loader::OBJ_FLAG_GC_OWNED;
    s->header.hash_code  = static_cast<uint32_t>(h);
    s->header.owner_pid  = 0;
    s->header.lock_depth = 0;
    s->header._mon_pad   = 0;

    s->encoding = static_cast<uint8_t>(enc);
    s->kind     = static_cast<uint8_t>(loader::StringKind::SLICE);
    s->_pad[0]  = s->_pad[1] = 0;
    s->length   = length;
    s->byte_len = byte_len;
    s->str_hash = 0;

    auto *sd = loader::str_slice(s);
    sd->parent_handle = parent;
    sd->byte_offset   = byte_offset;

    return h;
}

// =========================================================================
// Helper: interning automatico post-alocation
// =========================================================================

/**
 * @brief Intenta internar un string recien creado si es elegible.
 *
 * Solo se internan strings FLAT con byte_len <= STR_INTERN_THRESHOLD.
 * Si ya existe un identico, devuelve el GcHandle canonico del pool.
 *
 * @param vm       Proceso virtual.
 * @param h        GcHandle del string recien creado.
 * @param data     Puntero a los bytes del string.
 * @param byte_len Numero de bytes.
 * @param enc      Codificacion.
 * @return         GcHandle canonico (puede ser h u otro preexistente).
 */
static gc::GcHandle auto_intern(ProcessVM *vm, gc::GcHandle h,
                                 const uint8_t *data, uint32_t byte_len,
                                 loader::StringEncoding enc)
{
    if (byte_len > loader::STR_INTERN_THRESHOLD) return h; // demasiado largo para internar

    StringInternPool &pool = get_intern_pool(vm);
    std::string key        = make_intern_key(data, byte_len, enc);

    gc::GcHandle canonical = pool.intern(key, h);    // buscar o insertar

    if (canonical == h) {
        // recien insertado: marcar el objeto como internado
        uint8_t *payload = vm->gc_heap.deref(h);
        if (payload) {
            auto *s = reinterpret_cast<loader::StringObject *>(payload);
            s->kind |= loader::STR_INTERNED_FLAG; // activar flag de interning
        }
    }
    // si canonical != h, el objeto h es redundante pero el GC lo recolectara

    return canonical;
}

// =========================================================================
// Helper: materializar ROPE/SLICE a FLAT (flatten recursivo)
// =========================================================================

/**
 * @brief Acumula los bytes de un string (recursivo para ROPE) en un vector.
 *
 * @param vm    Proceso virtual.
 * @param h     GcHandle del string a recolectar.
 * @param out   Vector de salida donde se acumulan los bytes.
 * @return      true si la recoleccion tuvo exito.
 */
static bool collect_bytes(ProcessVM *vm, gc::GcHandle h, std::vector<uint8_t> &out) {
    if (h == gc::GC_NULL_HANDLE) return false;

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return false;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    using loader::StringKind;

    switch (loader::str_kind(s)) {
        case StringKind::FLAT: {
            const uint8_t *d = loader::str_data(s);
            out.insert(out.end(), d, d + s->byte_len); // copiar bytes del flat
            return true;
        }
        case StringKind::ROPE: {
            auto *rd = loader::str_rope(s);
            if (!collect_bytes(vm, rd->left_handle, out))  return false; // hijo izquierdo
            if (!collect_bytes(vm, rd->right_handle, out)) return false; // hijo derecho
            return true;
        }
        case StringKind::SLICE: {
            // re-deref necesario porque collect_bytes puede haber hecho allocs internos
            payload = vm->gc_heap.deref(h);
            if (!payload) return false;
            s  = reinterpret_cast<loader::StringObject *>(payload);
            auto *sd = loader::str_slice(s);

            uint8_t *pp = vm->gc_heap.deref(sd->parent_handle);
            if (!pp) return false;
            auto *parent = reinterpret_cast<loader::StringObject *>(pp);

            const uint8_t *d = loader::str_data(parent) + sd->byte_offset;
            out.insert(out.end(), d, d + s->byte_len); // copiar segmento del padre
            return true;
        }
    }
    return false;
}

/**
 * @brief Materializa cualquier string a FLAT, con interning automatico.
 *
 * - FLAT: devuelve el mismo handle (sin copia si ya esta bien).
 * - ROPE: recolecta todos los bytes recursivamente y crea un FLAT nuevo.
 * - SLICE: copia el segmento a un FLAT nuevo.
 *
 * Tras materializacion, el nuevo FLAT es internado si es elegible.
 *
 * @param vm  Proceso virtual.
 * @param h   GcHandle del string a materializar.
 * @return    GcHandle del FLAT resultante, o GC_NULL_HANDLE en fallo.
 */
static gc::GcHandle flatten_string(ProcessVM *vm, gc::GcHandle h) {
    if (h == gc::GC_NULL_HANDLE) return h;

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return gc::GC_NULL_HANDLE;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    if (loader::str_kind(s) == loader::StringKind::FLAT) return h; // ya es plano

    // recolectar todos los bytes en un vector
    std::vector<uint8_t> buf;
    buf.reserve(s->byte_len);
    if (!collect_bytes(vm, h, buf)) return gc::GC_NULL_HANDLE;

    auto enc = loader::str_encoding(s);
    uint32_t length = s->length; // preservar el numero de code points

    // crear el nuevo FLAT
    gc::GcHandle flat = alloc_flat(vm, buf.data(), static_cast<uint32_t>(buf.size()), length, enc);
    if (flat == gc::GC_NULL_HANDLE) return flat;

    // internado automatico del nuevo flat
    flat = auto_intern(vm, flat, buf.data(), static_cast<uint32_t>(buf.size()), enc);

    return flat;
}

// =========================================================================
// Helper: compactacion de codificacion (Compact Strings estilo HotSpot)
// =========================================================================

/**
 * @brief Detecta si un buffer puede representarse en una codificacion mas compacta.
 *
 * Reglas:
 *   - UTF-8  con todos los bytes <= 0x7F -> ASCII (ahorra mem, mismo puntero Win32)
 *   - UTF-16 con todos los code units <= 0x7F -> ASCII
 *   - UTF-16 con todos los code units <= 0xFF -> ANSI
 *
 * @param data      Buffer de entrada.
 * @param byte_len  Longitud en bytes.
 * @param enc       Codificacion de entrada.
 * @param[out] new_enc  Codificacion mas compacta posible.
 * @param[out] new_data Buffer recodificado (solo rellenado si cambia codificacion).
 * @param[out] new_len  Longitud del buffer recodificado.
 * @return  true si se puede compactar.
 */
static bool try_compact(const uint8_t *data, uint32_t byte_len,
                        loader::StringEncoding enc,
                        loader::StringEncoding &new_enc,
                        std::vector<uint8_t> &new_data,
                        uint32_t &new_len)
{
    if (enc == loader::StringEncoding::UTF8) {
        bool all_ascii = true;
        for (uint32_t i = 0; i < byte_len; ++i)
            if (data[i] > 0x7F) { all_ascii = false; break; }
        if (all_ascii) {
            new_enc  = loader::StringEncoding::ASCII; // compactar UTF-8 a ASCII
            new_data = std::vector<uint8_t>(data, data + byte_len);
            new_len  = byte_len;
            return true;
        }
    } else if (enc == loader::StringEncoding::UTF16) {
        if (byte_len % 2 != 0) return false; // longitud invalida para UTF-16
        bool all_ansi  = true;
        bool all_ascii = true;
        const uint16_t *u = reinterpret_cast<const uint16_t *>(data);
        uint32_t count    = byte_len / 2;
        for (uint32_t i = 0; i < count; ++i) {
            if (u[i] > 0xFF) { all_ansi  = false; break; }
            if (u[i] > 0x7F)   all_ascii = false;
        }
        if (all_ansi) {
            new_enc = all_ascii ? loader::StringEncoding::ASCII : loader::StringEncoding::ANSI;
            new_data.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                new_data[i] = static_cast<uint8_t>(u[i] & 0xFF); // extraer byte bajo
            new_len = count;
            return true;
        }
    }
    return false;
}

// =========================================================================
// Helper: calculo de code points para una codificacion y buffer
// =========================================================================

/**
 * @brief Cuenta los code points en un buffer segun la codificacion.
 *
 * @param data      Buffer de bytes.
 * @param byte_len  Longitud en bytes.
 * @param enc       Codificacion.
 * @return          Numero de code points.
 */
static uint32_t count_codepoints(const uint8_t *data, uint32_t byte_len,
                                  loader::StringEncoding enc)
{
    switch (enc) {
        case loader::StringEncoding::ASCII:
        case loader::StringEncoding::ANSI:
            return byte_len; // 1 byte por caracter
        case loader::StringEncoding::UTF8: {
            uint32_t count = 0;
            for (uint32_t i = 0; i < byte_len; ) {
                uint8_t b = data[i];
                if      ((b & 0x80) == 0)    { ++count; i += 1; }
                else if ((b & 0xE0) == 0xC0) { ++count; i += 2; }
                else if ((b & 0xF0) == 0xE0) { ++count; i += 3; }
                else                         { ++count; i += 4; }
            }
            return count;
        }
        case loader::StringEncoding::UTF16:
            return byte_len / 2; // aproximado (ignora surrogates)
        case loader::StringEncoding::UTF32:
            return byte_len / 4;
    }
    return byte_len;
}

// =========================================================================
// 0x46  STRMAKE r_dst, r_src, r_len, enc
// Encoding FIXED_4: [0x00][0x46][ctrl][byte3]
//   ctrl  = (r_dst<<4) | r_src    r_src = direccion VM del buffer
//   byte3 = (encoding<<4) | r_len  r_len = longitud en bytes
// =========================================================================

/**
 * @brief Ejecuta STRMAKE: crea un StringObject FLAT desde un buffer de VM.
 *
 * Aplica compactacion de codificacion automatica (HotSpot Compact Strings).
 * Interna el string automaticamente si byte_len <= STR_INTERN_THRESHOLD.
 */
void exec_instr_strmake(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t r_len = (instr.data_instruction.reg_data.reg2 >> 4) & 0xF; // nibble alto (emit_instr_three_reg)

    uint64_t vm_addr  = vm->registers.regs[r_src].qword();
    uint32_t byte_len = static_cast<uint32_t>(vm->registers.regs[r_len].qword());
    auto enc = loader::StringEncoding::UTF8; // entrada asumida UTF-8; try_compact ajusta si es ASCII

    // leer bytes desde la memoria VM al host
    std::vector<uint8_t> buf(byte_len);
    for (uint32_t i = 0; i < byte_len; ++i)
        buf[i] = vm->vm_mem.read_u8(vm_addr + i);

    // intentar compactacion de codificacion (HotSpot-style)
    loader::StringEncoding final_enc = enc;
    std::vector<uint8_t>   compact_buf;
    uint32_t compact_len = byte_len;
    bool compacted = try_compact(buf.data(), byte_len, enc, final_enc, compact_buf, compact_len);
    const uint8_t *final_data = compacted ? compact_buf.data() : buf.data();
    uint32_t final_byte_len   = compacted ? compact_len         : byte_len;

    uint32_t length = count_codepoints(final_data, final_byte_len, final_enc);

    gc::GcHandle h = alloc_flat(vm, final_data, final_byte_len, length, final_enc);
    if (h == gc::GC_NULL_HANDLE) {
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
        return;
    }

    h = auto_intern(vm, h, final_data, final_byte_len, final_enc); // internado automatico
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x55  STRMAKE_H r_dst, r_src, r_len
// Encoding FIXED_4: ctrl=(r_dst<<4)|r_src, byte3=(enc<<4)|r_len
// =========================================================================

/**
 * @brief Ejecuta STRMAKE_H: crea un StringObject FLAT desde un buffer HOST.
 *
 * Variante de STRMAKE (0x46) que lee directamente desde la memoria del
 * proceso host (puntero crudo, no direccion VM).  Util cuando el buffer
 * fuente proviene de @c malloc, @c gcallocp, @c str_cstr, etc.
 *
 * Aplica las mismas reglas que STRMAKE: compactacion HotSpot, internado
 * automatico, encoding por defecto UTF-8.
 */
void exec_instr_strmake_h(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t r_len = (instr.data_instruction.reg_data.reg2 >> 4) & 0xF;

    uint64_t host_addr = vm->registers.regs[r_src].qword();
    uint32_t byte_len  = static_cast<uint32_t>(vm->registers.regs[r_len].qword());
    auto enc = loader::StringEncoding::UTF8;

    // leer bytes desde memoria HOST (puntero crudo).  El usuario es responsable
    // de garantizar que [host_addr, host_addr+byte_len) sea memoria valida y
    // accesible; si no lo es, el deref disparara un AV capturable via try/catch
    // (FATAL_SEGMENTATION_FAULT en la instalacion del VEH/sigaction).
    std::vector<uint8_t> buf(byte_len);
    const uint8_t *src = reinterpret_cast<const uint8_t *>(host_addr);
    for (uint32_t i = 0; i < byte_len; ++i)
        buf[i] = src[i];

    loader::StringEncoding final_enc = enc;
    std::vector<uint8_t>   compact_buf;
    uint32_t compact_len = byte_len;
    bool compacted = try_compact(buf.data(), byte_len, enc, final_enc, compact_buf, compact_len);
    const uint8_t *final_data = compacted ? compact_buf.data() : buf.data();
    uint32_t final_byte_len   = compacted ? compact_len         : byte_len;

    uint32_t length = count_codepoints(final_data, final_byte_len, final_enc);

    gc::GcHandle h = alloc_flat(vm, final_data, final_byte_len, length, final_enc);
    if (h == gc::GC_NULL_HANDLE) {
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
        return;
    }

    h = auto_intern(vm, h, final_data, final_byte_len, final_enc);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x47  STRLEN r_dst, r_src
// =========================================================================

/** @brief Ejecuta STRLEN: devuelve el numero de code points del string. */
void exec_instr_strlen(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h   = static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword());
    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(s->length)); // valido para FLAT/ROPE/SLICE
}

// =========================================================================
// 0x48  STRCAT r_dst, r_a, r_b
// Encoding FIXED_4: ctrl=(r_dst<<4)|r_a, byte3=r_b
// =========================================================================

/**
 * @brief Ejecuta STRCAT: crea un nodo ROPE (concatenacion perezosa O(1)).
 *
 * Si uno de los operandos es un string vacio, devuelve el otro directamente
 * sin crear un nodo rope (optimizacion de identidad).
 * Si la concatenacion resultaria en un rope de profundidad > STR_ROPE_MAX_DEPTH,
 * materializa inmediatamente a FLAT para evitar degradacion de rendimiento.
 */
void exec_instr_strcat(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_a   =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t r_b   = (instr.data_instruction.reg_data.reg2 >> 4) & 0xF; // nibble alto (emit_instr_three_reg)

    gc::GcHandle ha = static_cast<gc::GcHandle>(vm->registers.regs[r_a].qword());
    gc::GcHandle hb = static_cast<gc::GcHandle>(vm->registers.regs[r_b].qword());

    uint8_t *pa = vm->gc_heap.deref(ha);
    uint8_t *pb = vm->gc_heap.deref(hb);

    if (!pa || !pb) {
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
        return;
    }

    auto *sa = reinterpret_cast<loader::StringObject *>(pa);
    auto *sb = reinterpret_cast<loader::StringObject *>(pb);

    // optimizacion de identidad: concatenar con string vacio devuelve el otro
    if (sa->byte_len == 0) { vm->registers.regs[r_dst].qword(hb); return; }
    if (sb->byte_len == 0) { vm->registers.regs[r_dst].qword(ha); return; }

    // calcular profundidad del nuevo rope
    uint32_t depth_a = 0, depth_b = 0;
    if (loader::str_kind(sa) == loader::StringKind::ROPE) depth_a = loader::str_rope(sa)->depth;
    if (loader::str_kind(sb) == loader::StringKind::ROPE) depth_b = loader::str_rope(sb)->depth;
    uint32_t new_depth = std::max(depth_a, depth_b) + 1;

    uint32_t total_len      = sa->length   + sb->length;
    uint32_t total_byte_len = sa->byte_len + sb->byte_len;
    auto enc = loader::str_encoding(sa); // hereda encoding del hijo izquierdo

    gc::GcHandle result;

    if (new_depth > loader::STR_ROPE_MAX_DEPTH) {
        // arbol demasiado profundo: materializar de inmediato
        gc::GcHandle fa = flatten_string(vm, ha);
        gc::GcHandle fb = flatten_string(vm, hb);
        uint8_t *pfa    = vm->gc_heap.deref(fa);
        uint8_t *pfb    = vm->gc_heap.deref(fb);
        if (!pfa || !pfb) { vm->registers.regs[r_dst].qword(gc::GC_NULL_HANDLE); return; }

        auto *sfa = reinterpret_cast<loader::StringObject *>(pfa);
        auto *sfb = reinterpret_cast<loader::StringObject *>(pfb);

        std::vector<uint8_t> buf(sfa->byte_len + sfb->byte_len);
        std::memcpy(buf.data(), loader::str_data(sfa), sfa->byte_len);
        std::memcpy(buf.data() + sfa->byte_len, loader::str_data(sfb), sfb->byte_len);

        result = alloc_flat(vm, buf.data(), static_cast<uint32_t>(buf.size()), total_len, enc);
        result = auto_intern(vm, result, buf.data(), static_cast<uint32_t>(buf.size()), enc);
    } else {
        // crear nodo ROPE perezoso
        result = alloc_rope(vm, ha, hb, total_len, total_byte_len, enc, new_depth);
    }

    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(result));
}

// =========================================================================
// 0x49  STRCMP r_dst, r_a, r_b
// =========================================================================

/**
 * @brief Ejecuta STRCMP: comparacion lexicografica de bytes entre dos strings.
 *
 * Materializa ROPE/SLICE antes de comparar.
 * r_dst recibe -1, 0 o 1 como int64_t con signo.
 * ZF=1 si iguales; SF=1 si a < b.
 */
void exec_instr_strcmp(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_a   =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t r_b   = (instr.data_instruction.reg_data.reg2 >> 4) & 0xF; // nibble alto (emit_instr_three_reg)

    gc::GcHandle ha = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_a].qword()));
    gc::GcHandle hb = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_b].qword()));

    uint8_t *pa = vm->gc_heap.deref(ha);
    uint8_t *pb = vm->gc_heap.deref(hb);

    if (!pa || !pb) {
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(-1LL));
        vm->registers.flags.bits.ZF = 0;
        vm->registers.flags.bits.SF = 1;
        return;
    }

    auto *sa = reinterpret_cast<loader::StringObject *>(pa);
    auto *sb = reinterpret_cast<loader::StringObject *>(pb);

    // comparacion rapida por hash si ambos estan calculados
    if (sa->str_hash != 0 && sb->str_hash != 0 && sa->str_hash != sb->str_hash) {
        int64_t result = -1LL; // hashes distintos => no iguales; asumir a < b
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(result));
        vm->registers.flags.bits.ZF = 0;
        vm->registers.flags.bits.SF = 1;
        return;
    }

    uint32_t min_len = std::min(sa->byte_len, sb->byte_len);
    int cmp = std::memcmp(loader::str_data(sa), loader::str_data(sb), min_len);
    if (cmp == 0) {
        if      (sa->byte_len < sb->byte_len) cmp = -1;
        else if (sa->byte_len > sb->byte_len) cmp =  1;
    }

    int64_t result = (cmp < 0) ? -1LL : (cmp > 0) ? 1LL : 0LL;
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(result));
    vm->registers.flags.bits.ZF = (result == 0);
    vm->registers.flags.bits.SF = (result < 0);
}

// =========================================================================
// 0x4A  STRCONV r_dst, r_src, enc
// =========================================================================

/**
 * @brief Ejecuta STRCONV: convierte la codificacion de un StringObject.
 *
 * Si la codificacion origen y destino coinciden devuelve el mismo handle.
 * Implementa conversion completa UTF-8 <-> UTF-16LE con surrogates.
 * Materializa ROPE/SLICE antes de convertir.
 */
void exec_instr_strconv(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst    = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src    =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t enc_bits = (instr.data_instruction.reg_data.reg2 >> 4) & 0xF; // nibble alto (emit_strconv)

    auto new_enc = static_cast<loader::StringEncoding>(enc_bits);
    gc::GcHandle hs = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));

    uint8_t *payload = vm->gc_heap.deref(hs);
    if (!payload) { vm->registers.regs[r_dst].qword(gc::GC_NULL_HANDLE); return; }

    auto *src_str = reinterpret_cast<loader::StringObject *>(payload);
    auto src_enc  = loader::str_encoding(src_str);

    if (src_enc == new_enc) { vm->registers.regs[r_dst].qword(hs); return; } // sin cambio

    const uint8_t *src_data = loader::str_data(src_str);
    uint32_t src_len = src_str->byte_len;
    gc::GcHandle result;

    if (src_enc == loader::StringEncoding::UTF8 && new_enc == loader::StringEncoding::UTF16) {
        // UTF-8 -> UTF-16LE
        std::vector<uint16_t> out;
        out.reserve(src_len);
        uint32_t i = 0;
        while (i < src_len) {
            uint32_t cp = 0;
            uint8_t b = src_data[i];
            if      ((b & 0x80) == 0)    { cp = b;                        i += 1; }
            else if ((b & 0xE0) == 0xC0) { cp = (b & 0x1F) << 6 | (src_data[i+1] & 0x3F); i += 2; }
            else if ((b & 0xF0) == 0xE0) { cp = (b & 0x0F) << 12 | (src_data[i+1] & 0x3F) << 6
                                               | (src_data[i+2] & 0x3F); i += 3; }
            else                         { cp = (b & 0x07) << 18 | (src_data[i+1] & 0x3F) << 12
                                               | (src_data[i+2] & 0x3F) << 6
                                               | (src_data[i+3] & 0x3F); i += 4; }
            if (cp < 0x10000) {
                out.push_back(static_cast<uint16_t>(cp));
            } else {
                cp -= 0x10000;
                out.push_back(static_cast<uint16_t>(0xD800 | (cp >> 10)));
                out.push_back(static_cast<uint16_t>(0xDC00 | (cp & 0x3FF)));
            }
        }
        uint32_t nb = static_cast<uint32_t>(out.size() * 2);
        result = alloc_flat(vm, reinterpret_cast<const uint8_t *>(out.data()), nb, src_str->length, loader::StringEncoding::UTF16);
    } else if (src_enc == loader::StringEncoding::UTF16 && new_enc == loader::StringEncoding::UTF8) {
        // UTF-16LE -> UTF-8
        std::vector<uint8_t> out;
        out.reserve(src_len);
        const uint16_t *units = reinterpret_cast<const uint16_t *>(src_data);
        uint32_t count = src_len / 2;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t cp = units[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count) {
                uint32_t lo = units[++i];
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (cp < 0x80) {
                out.push_back(static_cast<uint8_t>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<uint8_t>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<uint8_t>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<uint8_t>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
            }
        }
        uint32_t nb = static_cast<uint32_t>(out.size());
        result = alloc_flat(vm, out.data(), nb, src_str->length, loader::StringEncoding::UTF8);
    } else {
        // conversion generica: mismos bytes, nueva etiqueta
        result = alloc_flat(vm, src_data, src_len, src_str->length, new_enc);
    }

    if (result != gc::GC_NULL_HANDLE) {
        uint8_t *rp = vm->gc_heap.deref(result);
        if (rp) {
            auto *rs = reinterpret_cast<loader::StringObject *>(rp);
            result = auto_intern(vm, result, loader::str_data(rs), rs->byte_len, loader::str_encoding(rs));
        }
    }
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(result));
}

// =========================================================================
// 0x4B  STRRAW r_dst, r_src
// =========================================================================

/**
 * @brief Ejecuta STRRAW: devuelve la direccion host del buffer interno.
 *
 * Materializa ROPE/SLICE a FLAT antes de devolver el puntero.
 * El buffer siempre tiene un byte nulo extra al final para Win32 FFI.
 */
void exec_instr_strraw(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    vm->registers.regs[r_dst].qword(reinterpret_cast<uint64_t>(loader::str_data(s))); // puntero host al buffer
}

// =========================================================================
// 0x4C  STRSLICE r_dst, r_src, r_range
// Encoding FIXED_4: ctrl=(r_dst<<4)|r_src, byte3=r_range (nibble)
// r_range contiene (cp_start << 32) | cp_len como valor de 64 bits
// =========================================================================

/**
 * @brief Ejecuta STRSLICE: crea una vista SLICE sin copia sobre un string.
 *
 * r_range = (cp_start << 32) | cp_len (ambos en code points).
 * Si el padre es ROPE, se materializa primero (el slice solo puede apuntar a FLAT).
 * Si r_start + r_len > length del padre, se recorta al maximo valido.
 */
void exec_instr_strslice(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst   = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src   =  instr.data_instruction.reg_data.reg1       & 0xF;
    const uint8_t r_range = (instr.data_instruction.reg_data.reg2 >> 4)  & 0xF; // nibble alto (emit_instr_three_reg)

    gc::GcHandle parent_h = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));

    uint8_t *payload = vm->gc_heap.deref(parent_h);
    if (!payload) { vm->registers.regs[r_dst].qword(gc::GC_NULL_HANDLE); return; }

    auto *parent = reinterpret_cast<loader::StringObject *>(payload);
    auto enc = loader::str_encoding(parent);

    uint64_t range  = vm->registers.regs[r_range].qword();
    uint32_t cp_start = static_cast<uint32_t>(range >> 32);
    uint32_t cp_len   = static_cast<uint32_t>(range & 0xFFFFFFFFu);

    // clamp a los limites del padre
    if (cp_start >= parent->length) cp_start = parent->length;
    if (cp_start + cp_len > parent->length) cp_len = parent->length - cp_start;

    // calcular byte_offset y byte_len segun la codificacion
    uint32_t byte_offset = 0;
    uint32_t byte_len    = 0;
    const uint8_t *data  = loader::str_data(parent);

    switch (enc) {
        case loader::StringEncoding::ASCII:
        case loader::StringEncoding::ANSI:
            byte_offset = cp_start;
            byte_len    = cp_len;
            break;
        case loader::StringEncoding::UTF32:
            byte_offset = cp_start * 4;
            byte_len    = cp_len   * 4;
            break;
        case loader::StringEncoding::UTF16:
            byte_offset = cp_start * 2; // aproximado, ignora surrogates
            byte_len    = cp_len   * 2;
            break;
        case loader::StringEncoding::UTF8: {
            // para UTF-8 hay que contar bytes recorriendo
            uint32_t i = 0, cp = 0;
            while (cp < cp_start && i < parent->byte_len) {
                uint8_t b = data[i];
                if      ((b & 0x80) == 0)    { i += 1; }
                else if ((b & 0xE0) == 0xC0) { i += 2; }
                else if ((b & 0xF0) == 0xE0) { i += 3; }
                else                         { i += 4; }
                ++cp;
            }
            byte_offset = i;
            uint32_t j = i;
            cp = 0;
            while (cp < cp_len && j < parent->byte_len) {
                uint8_t b = data[j];
                if      ((b & 0x80) == 0)    { j += 1; }
                else if ((b & 0xE0) == 0xC0) { j += 2; }
                else if ((b & 0xF0) == 0xE0) { j += 3; }
                else                         { j += 4; }
                ++cp;
            }
            byte_len = j - i;
            break;
        }
    }

    // si el slice es todo el padre, devolver el padre directamente
    if (byte_offset == 0 && byte_len == parent->byte_len) {
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(parent_h));
        return;
    }

    gc::GcHandle h = alloc_slice(vm, parent_h, byte_offset, byte_len, cp_len, enc);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x4D  STRFLAT r_dst, r_src
// =========================================================================

/**
 * @brief Ejecuta STRFLAT: materializa ROPE/SLICE a FLAT.
 *
 * Si el origen ya es FLAT, devuelve el mismo handle sin copiar.
 * Tras materializacion aplica interning automatico.
 */
void exec_instr_strflat(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x4E  STRHASH r_dst, r_src
// =========================================================================

/**
 * @brief Ejecuta STRHASH: devuelve el hash FNV-1a del string.
 *
 * Materializa ROPE/SLICE antes de calcular.
 * El resultado se cachea en str_hash del StringObject.
 */
void exec_instr_strhash(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s  = reinterpret_cast<loader::StringObject *>(payload);
    uint32_t hv = loader::str_hash_compute(s); // calcula y cachea si necesario
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(hv));
}

// =========================================================================
// 0x4F  STRINTERN r_dst, r_src
// =========================================================================

/**
 * @brief Ejecuta STRINTERN: interna el string en el pool del proceso.
 *
 * Si ya existe un string identico en el pool, devuelve su GcHandle.
 * Solo strings FLAT pueden internarse; ROPE/SLICE se materializan primero.
 * r_dst puede ser distinto de r_src si el string ya estaba en el pool.
 */
void exec_instr_strintern(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h = flatten_string(vm, static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword()));

    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(gc::GC_NULL_HANDLE); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    auto enc = loader::str_encoding(s);
    const uint8_t *data = loader::str_data(s);

    h = auto_intern(vm, h, data, s->byte_len, enc); // interna y devuelve canonico
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x50  STRGETENC r_dst, r_src
// =========================================================================

/** @brief Ejecuta STRGETENC: devuelve el byte de codificacion del string. */
void exec_instr_strgetenc(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h   = static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword());
    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(s->encoding));
}

// =========================================================================
// 0x51  STRGETBYTES r_dst, r_src
// =========================================================================

/** @brief Ejecuta STRGETBYTES: devuelve el byte_len del string. */
void exec_instr_strgetbytes(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h   = static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword());
    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(s->byte_len));
}

// =========================================================================
// 0x52  STRGETKIND r_dst, r_src
// =========================================================================

/** @brief Ejecuta STRGETKIND: devuelve el kind del string (0=FLAT 1=ROPE 2=SLICE). */
void exec_instr_strgetkind(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_src =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h   = static_cast<gc::GcHandle>(vm->registers.regs[r_src].qword());
    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) { vm->registers.regs[r_dst].qword(0); return; }

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(s->kind & loader::STR_KIND_MASK));
}

// =========================================================================
// 0x53  STRRESERVE r_dst, r_capacity
// Encoding FIXED_4: ctrl=(r_dst<<4)|r_capacity
// =========================================================================

/**
 * @brief Ejecuta STRRESERVE: aloca un FLAT vacio con capacidad reservada.
 *
 * Crea un StringObject FLAT con byte_len=0 pero con capacidad interna
 * de r_capacity bytes.  El programa puede escribir directamente en el
 * buffer via STRRAW y luego llamar a STRFINALIZE para fijar la longitud.
 *
 * Util para el patron de string builder: reservar, escribir bytes, finalizar.
 */
void exec_instr_strreserve(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_cap =  instr.data_instruction.reg_data.reg1       & 0xF;

    uint32_t capacity = static_cast<uint32_t>(vm->registers.regs[r_cap].qword());

    // string vacio con capacidad reservada; encoding ASCII por defecto
    gc::GcHandle h = alloc_flat(vm, nullptr, 0, 0, loader::StringEncoding::ASCII, capacity);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

// =========================================================================
// 0x54  STRFINALIZE r_dst, r_newlen
// Encoding FIXED_4: ctrl=(r_dst<<4)|r_newlen
// =========================================================================

/**
 * @brief Ejecuta STRFINALIZE: actualiza byte_len, length y hash de un FLAT mutable.
 *
 * Debe usarse despues de STRRESERVE + escritura directa en el buffer para
 * registrar la longitud real del contenido.  El hash se recalcula.
 * Solo valido para strings FLAT; no opera sobre ROPE/SLICE.
 */
void exec_instr_strfinalize(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst    = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF;
    const uint8_t r_newlen =  instr.data_instruction.reg_data.reg1       & 0xF;

    gc::GcHandle h   = static_cast<gc::GcHandle>(vm->registers.regs[r_dst].qword());
    uint8_t *payload = vm->gc_heap.deref(h);
    if (!payload) return;

    auto *s = reinterpret_cast<loader::StringObject *>(payload);
    if (loader::str_kind(s) != loader::StringKind::FLAT) return; // solo para FLAT

    uint32_t new_byte_len = static_cast<uint32_t>(vm->registers.regs[r_newlen].qword());
    auto enc = loader::str_encoding(s);

    s->byte_len  = new_byte_len;
    s->length    = count_codepoints(loader::str_data(s), new_byte_len, enc);
    s->str_hash  = 0;              // invalida el cache para que se recalcule
    loader::str_hash_compute(s);   // calcular inmediatamente
    // garantizar terminador nulo tras el nuevo contenido
    loader::str_data(s)[new_byte_len] = 0;
}

// =========================================================================
// 0x6B  GETARGC   r_dst                  - argv: numero de argumentos
// 0x6C  GETARG    r_dst, r_idx           - argv: arg[i] como StringObject
// =========================================================================

/**
 * @brief Ejecuta GETARGC: deposita el numero de argumentos del script en r_dst.
 *
 * Lee `vm->scheduler.vm_reference.script_args.size()` y lo escribe como
 * uint64 en el registro destino.  Permite que un programa Vex consulte
 * argc via el builtin `args_count()` que baja a esta instruccion.
 *
 * No falla nunca; si no hay args, devuelve 0.
 *
 * @param vm    Proceso virtual.
 * @param instr Instruccion decodificada.  reg1 = registro destino.
 */
void exec_instr_getargc(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = instr.data_instruction.reg_data.reg1;
    const auto&   args  = vm->scheduler.vm_reference.script_args;
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(args.size()));
}

// =========================================================================
// 0x6E  GETMETHAT r_class, r_idx       - variante reg-reg de getmethod
// 0x6F  GETFLDAT  r_class, r_idx       - variante reg-reg de getfield
// =========================================================================
// El opcode existente getmethod 0xD9 toma idx como inmediato (rango 0..255)
// lo cual no permite iteracion dinamica desde Vex.  Estos opcodes nuevos
// toman idx en registro para que `getMethodAt(cls, i)` funcione con i
// runtime.  Mismo comportamiento: R00 = &cls->methods[idx] o 0 si fuera de
// rango / nulo.

void exec_instr_getmethat(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
    const uint8_t r_idx = instr.data_instruction.reg_data.reg2;
    auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
    const uint64_t idx = vm->registers.regs[r_idx].qword();
    if (cls == nullptr || idx >= cls->method_count) {
        vm->registers.regs[R00].qword(0);
        return;
    }
    vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->methods[idx]));
}

void exec_instr_getfldat(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
    const uint8_t r_idx = instr.data_instruction.reg_data.reg2;
    auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
    const uint64_t idx = vm->registers.regs[r_idx].qword();
    if (cls == nullptr || idx >= cls->field_count) {
        vm->registers.regs[R00].qword(0);
        return;
    }
    vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->fields[idx]));
}

/**
 * @brief Ejecuta GETARG: aloca un StringObject con el contenido del arg i-esimo.
 *
 * Lee el indice de `r_idx`, valida rango, y aloca un StringObject FLAT
 * en el GcHeap (via alloc_pinned) con los bytes del arg correspondiente.
 * Encoding por defecto: UTF-8 (la VM asume args UTF-8 desde main.cpp).
 *
 * En caso de indice fuera de rango o fallo de alocacion, devuelve
 * GC_NULL_HANDLE (0) que el frontend Vex interpretara como string vacio /
 * nulo (la verificacion de rango debe hacerla el llamador con
 * `args_count()` antes).
 *
 * @param vm    Proceso virtual.
 * @param instr Instruccion decodificada.  reg1 = dst, reg2 = idx.
 */
void exec_instr_getarg(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_dst = instr.data_instruction.reg_data.reg1;
    const uint8_t r_idx = instr.data_instruction.reg_data.reg2;
    const auto&   args  = vm->scheduler.vm_reference.script_args;
    const uint64_t idx  = vm->registers.regs[r_idx].qword();

    if (idx >= args.size()) {
        vm->registers.regs[r_dst].qword(0);  // GC_NULL_HANDLE
        return;
    }

    const std::string& s = args[idx];
    // Conteo de code points UTF-8.  Para ASCII puro coincide con byte_len;
    // para UTF-8 multi-byte lo recalcula la helper count_codepoints.
    const auto* data = reinterpret_cast<const uint8_t*>(s.data());
    uint32_t byte_len = static_cast<uint32_t>(s.size());
    uint32_t length   = count_codepoints(data, byte_len,
                                         loader::StringEncoding::UTF8);

    gc::GcHandle h = alloc_flat(vm, data, byte_len, length,
                                loader::StringEncoding::UTF8);
    vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));
}

} // namespace runtime
