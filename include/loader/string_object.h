/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef STRING_OBJECT_H
#define STRING_OBJECT_H

/**
 * @file string_object.h
 * @brief Definicion de StringObject y el sistema de cadenas de VestaVM.
 *
 * == Arquitectura del sistema de cadenas ==
 *
 * Cada StringObject puede adoptar uno de tres modos de almacenamiento
 * (campo @c kind del byte en offset 25):
 *
 *   FLAT  (kind=0): buffer contiguo de bytes inmediatamente despues de la
 *          cabecera (offset 40+).  Siempre termina con un byte nulo extra
 *          para compatibilidad con la Win32 API (*A / *W functions).
 *          Los strings cortos (<= 255 bytes) son SSO implicito: un unico
 *          bloque GC contiene cabecera y datos sin indirecciones.
 *
 *   ROPE  (kind=1): nodo de un arbol de concatenacion lazily evaluado.
 *          Los datos en offset 40 son un struct RopeData con los GcHandles
 *          de los hijos izquierdo y derecho.  Se materializa a FLAT con
 *          la instruccion STRFLAT o automaticamente al llamar a STRRAW.
 *          Permite concatenacion O(1) sin copias.
 *
 *   SLICE (kind=2): vista sin copia sobre un segmento de otro string FLAT.
 *          Los datos en offset 40 son un struct SliceData con el GcHandle
 *          del padre y el offset de inicio en bytes.  La instruccion
 *          STRSLICE crea vistas O(1) sobre cualquier substring.
 *
 * == Compact Strings (estilo HotSpot) ==
 *
 * STRMAKE detecta automaticamente si el contenido UTF-8 cabe en ASCII y
 * compacta la codificacion.  UTF-16 con todos los code units <= 0xFF se
 * compacta a ANSI.  Esto reduce a la mitad el uso de memoria para textos
 * Latin-1 sin impacto en la API de usuario.
 *
 * == Small String Optimization (SSO) ==
 *
 * Todos los FLAT strings almacenan sus datos inline en el mismo bloque GC
 * que la cabecera, sin ningun puntero extra.  Para strings cortos (un
 * registro de 64 bits puede representar hasta 7 bytes UTF-8) el compilador
 * puede optar por usar registros directamente; para el resto, el unico coste
 * es el bloque GC de tamano sizeof(StringObject) + byte_len + 1.
 *
 * == Hash cacheado + incremental ==
 *
 * El campo @c str_hash almacena el hash FNV-1a del buffer de bytes.
 * El hash se calcula la primera vez que se necesita y se cachea.
 * En STRMAKE se calcula inmediatamente si byte_len <= 64 (umbral de
 * "string corto") para acelerar las comparaciones posteriores.
 * Para concatenaciones de ROPE, el hash se calcula sobre el buffer
 * materializado en STRFLAT.
 *
 * == Interning ==
 *
 * La instruccion STRINTERN busca el string en el pool del proceso.
 * Si existe un string identico, devuelve el GcHandle del existente
 * (sin crear un nuevo objeto).  El pool usa una tabla hash por proceso
 * con clave = raw bytes + encoding.  Solo se internan strings FLAT.
 *
 * == Compatibilidad Win32 ==
 *
 *   ASCII / ANSI  : STRRAW devuelve char*  apto para *A functions.
 *   UTF-16LE      : STRRAW devuelve WCHAR* apto para *W functions.
 *   UTF-8         : STRRAW devuelve char*  compatible con MultiByteToWideChar.
 *
 * STRRAW materializa automaticamente ROPE/SLICE antes de devolver el ptr.
 *
 * == Layout de memoria ==
 * @code
 *   offset  0..23  ObjectHeader  (class_ptr, flags, hash_code, owner_pid, lock_depth, _mon_pad)
 *   offset 24      encoding      uint8_t - StringEncoding (ASCII=0 ANSI=1 UTF8=2 UTF16=3 UTF32=4)
 *   offset 25      kind          uint8_t - StringKind + flags de interning (bit7=is_interned)
 *   offset 26..27  _pad[2]       uint8_t[2]
 *   offset 28      length        uint32_t - numero de code points
 *   offset 32      byte_len      uint32_t - numero de bytes del contenido logico
 *   offset 36      str_hash      uint32_t - hash FNV-1a cacheado (0=sin calcular)
 *   offset 40..    data[]        segun kind: bytes / RopeData / SliceData
 * @endcode
 */

#include "loader/oop_types.h"
#include <cstdint>

namespace loader {

    // =========================================================================
    // Enumeraciones
    // =========================================================================

    /**
     * @brief Codificacion del buffer de texto de StringObject.
     *
     * Los valores son parte del formato .velb y no deben reordenarse.
     */
    enum class StringEncoding : uint8_t {
        ASCII  = 0, ///< US-ASCII 7 bits; 1 byte/char; compatible con ANSI [0..127]
        ANSI   = 1, ///< Windows-1252 / Latin-1; 1 byte/char
        UTF8   = 2, ///< Unicode UTF-8; 1-4 bytes/code point (longitud variable)
        UTF16  = 3, ///< Unicode UTF-16 Little-Endian; 2 o 4 bytes/code point
        UTF32  = 4  ///< Unicode UTF-32 Little-Endian; 4 bytes/code point (ancho fijo)
    };

    /**
     * @brief Modo de almacenamiento interno de StringObject.
     *
     * Ocupa los bits [1:0] del byte @c kind del StringObject.
     * El bit 7 de @c kind es el flag @c is_interned (independiente del kind).
     */
    enum class StringKind : uint8_t {
        FLAT  = 0, ///< Buffer contiguo inline (SSO implicito para strings cortos)
        ROPE  = 1, ///< Nodo de arbol de concatenacion (lazy, O(1) concat)
        SLICE = 2  ///< Vista sin copia sobre un FLAT padre (O(1) substring)
    };

    /** @brief Mascara del campo kind en el byte kind. */
    static constexpr uint8_t STR_KIND_MASK     = 0x03;
    /** @brief Flag de interning en el byte kind (bit 7). */
    static constexpr uint8_t STR_INTERNED_FLAG = 0x80;
    /** @brief Umbral de bytes para interning automatico en STRMAKE. */
    static constexpr uint32_t STR_INTERN_THRESHOLD = 64;
    /** @brief Umbral de profundidad del arbol rope para rebalanceo (no implementado aun). */
    static constexpr uint32_t STR_ROPE_MAX_DEPTH = 48;

    // =========================================================================
    // Datos especificos por kind (solapan data[] en offset 40)
    // =========================================================================

    /**
     * @brief Datos de un nodo ROPE en offset 40 del StringObject.
     *
     * left y right son GcHandles de StringObject hijos.  Ambos pueden ser
     * de cualquier kind (FLAT, ROPE o SLICE).  La profundidad del arbol se
     * usa como heuristica para decidir cuando materializar.
     *
     * Nota de seguridad GC: el GC de VestaVM puede mover objetos durante
     * la evacuacion de la Nursery.  Los GcHandles son indirecciones estables
     * a traves de la HandleTable; el llamante debe mantener referencias vivas
     * a los hijos (p.ej. en registros) mientras el rope no este materializado.
     */
    struct RopeData {
        uint32_t left_handle;  ///< GcHandle del hijo izquierdo (string A)
        uint32_t right_handle; ///< GcHandle del hijo derecho  (string B)
        uint32_t depth;        ///< Profundidad del subarbol (hoja=0)
        uint32_t _pad;         ///< Relleno a 16 bytes
    };

    /**
     * @brief Datos de un SLICE (vista sin copia) en offset 40 del StringObject.
     *
     * parent_handle es el GcHandle del StringObject FLAT padre.
     * byte_offset es el offset en bytes dentro del buffer del padre.
     * Los campos @c length y @c byte_len del StringObject describen
     * el tamano de la vista.
     */
    struct SliceData {
        uint32_t parent_handle; ///< GcHandle del StringObject FLAT fuente
        uint32_t byte_offset;   ///< Offset en bytes dentro del buffer del padre
    };

    // =========================================================================
    // StringObject
    // =========================================================================

    /**
     * @brief Objeto cadena de texto gestionado por el GC de VestaVM.
     *
     * Siempre se aloca con @c gc::GcHeap::alloc() y se referencia via GcHandle.
     * El tamano de alocation es @c sizeof(StringObject) + tamano_extra donde
     * tamano_extra depende del kind:
     *   FLAT  : byte_len + 1 (buffer de datos + byte nulo extra)
     *   ROPE  : sizeof(RopeData)
     *   SLICE : sizeof(SliceData)
     */
    struct alignas(8) StringObject {
        ObjectHeader header;   ///< Cabecera GC (24 bytes)
        uint8_t      encoding; ///< Codificacion del texto (StringEncoding)
        uint8_t      kind;     ///< StringKind (bits[1:0]) + is_interned (bit7)
        uint8_t      _pad[2];  ///< Relleno de alineacion
        uint32_t     length;   ///< Numero de code points (caracteres logicos)
        uint32_t     byte_len; ///< Numero de bytes del contenido logico
        uint32_t     str_hash; ///< Hash FNV-1a cacheado (0 = sin calcular)
        // data[] a partir de offset 40 (segun kind)
    };

    static_assert(sizeof(StringObject) == 40,
        "StringObject debe tener exactamente 40 bytes");

    // =========================================================================
    // Funciones helper inline
    // =========================================================================

    /** @brief Devuelve el kind de un StringObject. */
    inline StringKind str_kind(const StringObject *s) {
        return static_cast<StringKind>(s->kind & STR_KIND_MASK);
    }

    /** @brief Devuelve la codificacion de un StringObject. */
    inline StringEncoding str_encoding(const StringObject *s) {
        return static_cast<StringEncoding>(s->encoding);
    }

    /** @brief Comprueba si el string esta en el pool de interning. */
    inline bool str_is_interned(const StringObject *s) {
        return (s->kind & STR_INTERNED_FLAG) != 0;
    }

    /** @brief Devuelve puntero al area data[] (primeros bytes despues de la cabecera). */
    inline uint8_t *str_data(StringObject *s) {
        return reinterpret_cast<uint8_t *>(s) + sizeof(StringObject);
    }
    inline const uint8_t *str_data(const StringObject *s) {
        return reinterpret_cast<const uint8_t *>(s) + sizeof(StringObject);
    }

    /** @brief Devuelve puntero a RopeData (solo valido si kind==ROPE). */
    inline RopeData *str_rope(StringObject *s) {
        return reinterpret_cast<RopeData *>(str_data(s));
    }

    /** @brief Devuelve puntero a SliceData (solo valido si kind==SLICE). */
    inline SliceData *str_slice(StringObject *s) {
        return reinterpret_cast<SliceData *>(str_data(s));
    }

    /**
     * @brief Calcula (y cachea) el hash FNV-1a del buffer de bytes del string.
     *
     * Solo es valido para strings FLAT.  Para ROPE/SLICE devuelve 0 sin
     * modificar str_hash (deben materializarse primero).
     *
     * @param s Puntero al StringObject FLAT.
     * @return  Hash FNV-1a de 32 bits (nunca devuelve 0 para strings no vacios).
     */
    inline uint32_t str_hash_compute(StringObject *s) {
        if (str_kind(s) != StringKind::FLAT) return 0; // rope/slice: sin hash directo
        if (s->str_hash != 0) return s->str_hash;       // ya cacheado

        // Sprint string-perf-3 (2026-06-02): FNV-1a 64-bit en lugar de
        // 32-bit.  En x86-64 una sola multiplicacion 64x64=64 cuesta lo
        // mismo que la 32x32=32 (mismo unit/latency en CPUs modernos)
        // pero distribuye mejor + acepta el mismo algoritmo que los
        // fast paths en exec_instruction_string.cpp (STRMAKE/STRCAT).
        // El campo str_hash es uint32_t -> truncamos al low half.
        const uint8_t *data = str_data(s);
        uint64_t h64        = 1469598103934665603ULL;  // FNV-1a 64-bit offset
        for (uint32_t i = 0; i < s->byte_len; ++i) {
            h64 ^= data[i];
            h64 *= 1099511628211ULL;                    // FNV-1a 64-bit prime
        }
        uint32_t h = static_cast<uint32_t>(h64 & 0xFFFFFFFFu);
        if (h == 0) h = 1;        // 0 es centinela; usar 1 en su lugar
        s->str_hash = h;
        return h;
    }

} // namespace loader

#endif // STRING_OBJECT_H
