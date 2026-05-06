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
 * @file vesta_collections.c
 * @brief Implementacion de las colecciones nativas de VestaVM.
 *
 * Cuatro estructuras de datos de proposito general:
 *
 *   VestaList      -- array dinamico de uint32_t (GcHandle).
 *   VestaArrayList -- array dinamico de uint64_t (raw value o GcHandle).
 *   VestaHashMap   -- tabla hash uint64_t->uint64_t, open addressing lineal.
 *   VestaHashSet   -- conjunto uint64_t, envoltura sobre VestaHashMap.
 *
 * Todas las estructuras viven en memoria host (malloc/realloc/free).
 * Los handles devueltos al bytecode son punteros cast a uint64_t.
 *
 * Hash de clave: FNV-1a de 64 bits para distribucion uniforme sin division.
 * Factor de carga maximo de HashMap/Set: 75% (rehash al superar el umbral).
 * Factor de crecimiento de List/ArrayList: x2 desde la capacidad actual.
 *
 * @def VCOL_COLLECTIONS_DEBUG
 *   Define a 1 para activar mensajes de diagnostico en stderr.
 *   Valor 0 por defecto (produccion).
 */

#ifndef VCOL_COLLECTIONS_DEBUG
#  define VCOL_COLLECTIONS_DEBUG 0
#endif

#include "vesta_collections.h"
#include "../../../include/ffi/vesta_plugin.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if VCOL_COLLECTIONS_DEBUG
#  include <stdio.h>
#  define VCOL_DBG(fmt, ...) fprintf(stderr, "[vcol] " fmt "\n", ##__VA_ARGS__)
#else
#  define VCOL_DBG(fmt, ...)
#endif

/* =========================================================================
 * Estado global del plugin
 * ========================================================================= */

/** @brief Puntero a la API de VestaVM, recibido en vesta_init(). */
static const VestaPluginAPI *g_api = NULL;

/* =========================================================================
 * Utilidades internas
 * ========================================================================= */

/**
 * @brief Devuelve la potencia de 2 mayor o igual a n.
 * @param n Valor de entrada (>=1).
 * @return Potencia de 2 >= n.
 */
static uint64_t next_pow2(uint64_t n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/* fnv1a_u64 eliminado en A.25: el HashMap usa wyhash64 (1 mul128 + 1 xor)
   que es 8x mas rapido que el FNV-1a byte-a-byte (8 muls + 8 xors).  Si
   alguna otra parte futura del plugin necesita FNV-1a, restaurar aqui. */

/* =========================================================================
 * VestaList -- array dinamico de uint32_t
 * ========================================================================= */

/** @brief Capacidad minima de una VestaList. */
#define VLIST_MIN_CAP 8u

/**
 * @brief Estructura interna de VestaList.
 *
 * Los datos se almacenan como uint32_t para compactar GcHandles (que son
 * indices de 32 bits).  El handle expuesto al bytecode es VestaList* cast.
 */
typedef struct VestaList {
    uint32_t *data; /**< array de GcHandles (uint32_t cada uno) */
    uint64_t  size; /**< numero de elementos actuales */
    uint64_t  cap;  /**< capacidad en slots uint32_t */
} VestaList;

/**
 * @brief Crece la lista al doble de capacidad si size == cap.
 * @return 1 si ok, 0 si fallo malloc.
 */
static int vlist_grow(VestaList *l) {
    uint64_t  new_cap  = l->cap * 2;
    uint32_t *new_data = (uint32_t *) realloc(l->data,
                                              new_cap * sizeof(uint32_t));
    if (!new_data) return 0;
    l->data = new_data;
    l->cap  = new_cap;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_new(uint64_t initial_capacity) {
    uint64_t cap = initial_capacity < VLIST_MIN_CAP
                       ? VLIST_MIN_CAP
                       : initial_capacity;
    VestaList *l = (VestaList *) malloc(sizeof(VestaList));
    if (!l) return VCOL_NULL;
    l->data = (uint32_t *) malloc(cap * sizeof(uint32_t));
    if (!l->data) {
        free(l);
        return VCOL_NULL;
    }
    l->size = 0;
    l->cap  = cap;
    VCOL_DBG("list_new cap=%llu -> %p", (unsigned long long)cap, (void *)l);
    return (uint64_t) (uintptr_t) l;
}

VESTA_PLUGIN_EXPORT void vcol_list_free(uint64_t handle) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l) return;
    free(l->data);
    free(l);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_push(uint64_t handle, uint64_t element) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l) return VCOL_NOT_FOUND;
    if (l->size == l->cap && !vlist_grow(l)) return VCOL_NOT_FOUND;
    l->data[l->size++] = (uint32_t) (element & 0xFFFFFFFFu); /* almacenar 32 bits bajos */
    return l->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_pop(uint64_t handle) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l || l->size == 0) return VCOL_NOT_FOUND;
    return (uint64_t) l->data[--l->size];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_get(uint64_t handle, uint64_t index) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    return (uint64_t) l->data[index];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_set(uint64_t handle, uint64_t index,
                                           uint64_t element) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    uint64_t old   = (uint64_t) l->data[index];
    l->data[index] = (uint32_t) (element & 0xFFFFFFFFu);
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_size(uint64_t handle) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    return l ? l->size : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_cap(uint64_t handle) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    return l ? l->cap : 0;
}

VESTA_PLUGIN_EXPORT void vcol_list_clear(uint64_t handle) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (l) l->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_remove_at(uint64_t handle, uint64_t index) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    uint64_t old = (uint64_t) l->data[index];
    /* desplazar elementos a la izquierda */
    memmove(&l->data[index], &l->data[index + 1],
            (l->size - index - 1) * sizeof(uint32_t));
    l->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_insert(uint64_t handle, uint64_t index,
                                              uint64_t element) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l) return VCOL_NOT_FOUND;
    if (index > l->size) index = l->size; /* clamp al final */
    if (l->size == l->cap && !vlist_grow(l)) return VCOL_NOT_FOUND;
    /* desplazar elementos a la derecha */
    memmove(&l->data[index + 1], &l->data[index],
            (l->size - index) * sizeof(uint32_t));
    l->data[index] = (uint32_t) (element & 0xFFFFFFFFu);
    l->size++;
    return l->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_indexof(uint64_t handle, uint64_t element) {
    VestaList *l = (VestaList *) (uintptr_t) handle;
    if (!l) return VCOL_NOT_FOUND;
    uint32_t target = (uint32_t) (element & 0xFFFFFFFFu);
    for (uint64_t i = 0; i < l->size; i++) {
        if (l->data[i] == target) return i;
    }
    return VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_clone(uint64_t handle) {
    VestaList *src = (VestaList *) (uintptr_t) handle;
    if (!src) return VCOL_NULL;
    uint64_t h = vcol_list_new(src->cap);
    if (!h) return VCOL_NULL;
    VestaList *dst = (VestaList *) (uintptr_t) h;
    memcpy(dst->data, src->data, src->size * sizeof(uint32_t));
    dst->size = src->size;
    return h;
}

/* =========================================================================
 * VestaArrayList -- array dinamico de uint64_t
 * ========================================================================= */

/** @brief Capacidad minima de un VestaArrayList. */
#define VALIST_MIN_CAP 8u

/**
 * @brief Estructura interna de VestaArrayList.
 *
 * Almacena uint64_t directamente: permite mezclar raw values y GcHandles
 * de 64 bits sin perdida de precision.
 */
typedef struct VestaArrayList {
    uint64_t *data; /**< array de uint64_t */
    uint64_t  size; /**< numero de elementos */
    uint64_t  cap;  /**< capacidad en slots */
} VestaArrayList;

static int valist_grow(VestaArrayList *al) {
    uint64_t  new_cap  = al->cap * 2;
    uint64_t *new_data = (uint64_t *) realloc(al->data,
                                              new_cap * sizeof(uint64_t));
    if (!new_data) return 0;
    al->data = new_data;
    al->cap  = new_cap;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_new(uint64_t initial_capacity) {
    uint64_t cap = initial_capacity < VALIST_MIN_CAP
                       ? VALIST_MIN_CAP
                       : initial_capacity;
    VestaArrayList *al = (VestaArrayList *) malloc(sizeof(VestaArrayList));
    if (!al) return VCOL_NULL;
    al->data = (uint64_t *) malloc(cap * sizeof(uint64_t));
    if (!al->data) {
        free(al);
        return VCOL_NULL;
    }
    al->size = 0;
    al->cap  = cap;
    return (uint64_t) (uintptr_t) al;
}

VESTA_PLUGIN_EXPORT void vcol_alist_free(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al) return;
    free(al->data);
    free(al);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_push(uint64_t handle, uint64_t value) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al) return VCOL_NOT_FOUND;
    if (al->size == al->cap && !valist_grow(al)) return VCOL_NOT_FOUND;
    al->data[al->size++] = value;
    return al->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_pop(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al || al->size == 0) return VCOL_NOT_FOUND;
    return al->data[--al->size];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_get(uint64_t handle, uint64_t index) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    return al->data[index];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_set(uint64_t handle, uint64_t index,
                                            uint64_t value) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    uint64_t old    = al->data[index];
    al->data[index] = value;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_size(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    return al ? al->size : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_cap(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    return al ? al->cap : 0;
}

VESTA_PLUGIN_EXPORT void vcol_alist_clear(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (al) al->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_remove_at(uint64_t handle, uint64_t index) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    uint64_t old = al->data[index];
    memmove(&al->data[index], &al->data[index + 1],
            (al->size - index - 1) * sizeof(uint64_t));
    al->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_insert(uint64_t handle, uint64_t index,
                                               uint64_t value) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al) return VCOL_NOT_FOUND;
    if (index > al->size) index = al->size;
    if (al->size == al->cap && !valist_grow(al)) return VCOL_NOT_FOUND;
    memmove(&al->data[index + 1], &al->data[index],
            (al->size - index) * sizeof(uint64_t));
    al->data[index] = value;
    al->size++;
    return al->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_indexof(uint64_t handle, uint64_t value) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al) return VCOL_NOT_FOUND;
    for (uint64_t i = 0; i < al->size; i++) {
        if (al->data[i] == value) return i;
    }
    return VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_clone(uint64_t handle) {
    VestaArrayList *src = (VestaArrayList *) (uintptr_t) handle;
    if (!src) return VCOL_NULL;
    uint64_t h = vcol_alist_new(src->cap);
    if (!h) return VCOL_NULL;
    VestaArrayList *dst = (VestaArrayList *) (uintptr_t) h;
    memcpy(dst->data, src->data, src->size * sizeof(uint64_t));
    dst->size = src->size;
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *) a;
    uint64_t ub = *(const uint64_t *) b;
    return (ua > ub) - (ua < ub); /* sin overflow */
}

VESTA_PLUGIN_EXPORT void vcol_alist_sort(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (!al || al->size < 2) return;
    qsort(al->data, (size_t) al->size, sizeof(uint64_t), cmp_u64);
}

/* =========================================================================
 * VestaHashMap -- swisstable-style con SSE2 + wyhash64
 *
 * Reescrito en A.25 con design hardware-aware:
 *   - Hash: wyhash64 (1 mul128 + 1 xor) en lugar de FNV-1a (8 muls).
 *   - Layout: grupos de 16 slots con control bytes adyacentes (~4 cache lines
 *     por grupo).  Un control byte por slot encodea: 0x80=empty, 0xFE=deleted,
 *     0..0x7F=h2 (los 7 bits altos del hash usados como tag rapido).
 *   - Lookup: SSE2 _mm_cmpeq_epi8 escanea los 16 control bytes en paralelo
 *     comparando con h2.  Para cada bit en el bitmask resultante, una sola
 *     comparacion de key 64-bit.  Skip rapido a slots empty (early-exit).
 *   - Probing: cuadratico por grupo (g, g+1, g+3, g+6, ...) -- triangular
 *     numbers -- garantiza visitar todos los grupos sin patron pegajoso.
 *   - Carga: 87.5% (14/16 slots ocupados antes de rehash) vs 75% del FNV.
 *   - Sin tombstones: el delete usa tombstone solo si es necesario para
 *     mantener la cadena (mas eficiente: comprueba si el grupo tiene algun
 *     empty; si si, el delete puede ser un empty real).
 *
 * Coste medio (carga 87.5%):
 *   - get/contains: 1.0-1.05 grupos visitados, ~1.5 cmps de key amortizado.
 *   - put: igual + 1 vez al rehash cada N inserciones.
 * Coste vs FNV+linear: ~2-4x mejora en lookup denso (esperado).
 * ========================================================================= */

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#  include <emmintrin.h>
#  define VMAP_HAVE_SSE2 1
#else
#  define VMAP_HAVE_SSE2 0
#endif

/* Atributo de alineacion portable a GCC/Clang/MinGW.  C11 alignas requiere
   <stdalign.h> y sintaxis especifica que MinGW no acepta antes del nombre
   del struct.  __attribute__((aligned(N))) tras el cierre del struct es
   universalmente soportado por todos los compiladores que VestaVM usa. */
#define VMAP_ALIGN16 __attribute__((aligned(16)))

/** @brief Capacidad minima de un VestaHashMap (en slots, potencia de 2 >=16). */
#define VMAP_MIN_CAP 16u

/** @brief Slots por grupo (forzado a 16 para SSE2 _mm_cmpeq_epi8). */
#define VMAP_GROUP_W 16u

/** @brief Control byte: slot vacio (bit 7 set distingue de h2). */
#define VMAP_CTRL_EMPTY    ((uint8_t)0x80)
/** @brief Control byte: slot con tombstone (delete que rompe cadena). */
#define VMAP_CTRL_DELETED  ((uint8_t)0xFE)

/**
 * @brief Hash wyhash64 inline para clave uint64_t.
 *
 * Un solo mul128 + xor.  Mucho mas rapido que FNV-1a (8 muls) y con calidad
 * estadistica equivalente o mejor para distribuciones reales.  El golden
 * ratio 0x9E3779B97F4A7C15 actua como semilla fija; cualquier modificacion
 * cambiaria el orden de iteracion observable de keys() / values() pero no
 * la correctitud.
 *
 * @param key Clave a hashear.
 * @return Hash de 64 bits con buena entropia en todos los bits.
 */
static inline uint64_t wyhash64_u64(uint64_t key) {
    static const uint64_t SEED = 0x9E3779B97F4A7C15ULL; /* golden ratio */
    __uint128_t           r    = (__uint128_t) (key ^ SEED) * (__uint128_t) SEED;
    return (uint64_t) r ^ (uint64_t) (r >> 64);
}

/** @brief h1: bits altos del hash, usados para indice de grupo. */
static inline uint64_t vmap_h1(uint64_t hash) {
    return hash >> 7;
}

/** @brief h2: bits bajos (7 bits) del hash, control byte de cada slot. */
static inline uint8_t vmap_h2(uint64_t hash) {
    return (uint8_t) (hash & 0x7F);
}

/**
 * @brief Grupo de 16 slots.  Layout interleaved:
 *   [ ctrl[16]: 16 B | keys[16]: 128 B | vals[16]: 128 B ] = 272 B (~4.25 lineas).
 * El acceso a ctrl es vectorial (16 bytes en 1 carga SSE2); luego, para los
 * 1-3 matches probables se accede a keys[i].
 *
 * alignas(16) garantiza que ctrl este alineado para _mm_load_si128 (no
 * estrictamente requerido por _mm_loadu_si128, pero acelera el load).
 */
typedef struct VMapGroup {
    uint8_t  ctrl[VMAP_GROUP_W];
    uint64_t keys[VMAP_GROUP_W];
    uint64_t vals[VMAP_GROUP_W];
}
        VMAP_ALIGN16 VMapGroup;

/**
 * @brief Estructura interna de VestaHashMap (swisstable-style).
 */
typedef struct VestaHashMap {
    VMapGroup *groups;    /**< array de grupos (siempre potencia de 2). */
    uint64_t   group_cnt; /**< numero de grupos (cada uno 16 slots). */
    uint64_t   size;      /**< pares validos. */
    uint64_t   threshold; /**< rehash cuando size >= threshold (87.5% del total). */
} VestaHashMap;

/**
 * @brief Numero total de slots de la tabla.
 */
static inline uint64_t vmap_capacity(const VestaHashMap *m) {
    return m->group_cnt * VMAP_GROUP_W;
}

/**
 * @brief Devuelve mascara de bytes que coinciden con @p needle en @p ctrl[16].
 * Cada bit del retorno (16 bits utiles) indica si el ctrl correspondiente
 * coincide con needle.  Implementacion SSE2: 1 load + 1 cmp + 1 movemask.
 */
static inline uint32_t vmap_match_ctrl(const uint8_t *ctrl, uint8_t needle) {
#if VMAP_HAVE_SSE2
    __m128i v_ctrl = _mm_loadu_si128((const __m128i *) ctrl);
    __m128i v_need = _mm_set1_epi8((char) needle);
    __m128i v_eq   = _mm_cmpeq_epi8(v_ctrl, v_need);
    return (uint32_t) _mm_movemask_epi8(v_eq);
#else
    /* Fallback escalar: compara byte a byte (lento, solo para no-x86). */
    uint32_t bits = 0;
    for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
        if (ctrl[i] == needle) bits |= (1u << i);
    }
    return bits;
#endif
}

/**
 * @brief Inserta (key,value) en una tabla nueva durante rehash.
 * Garantiza que no hay tombstones ni duplicados (recibe entradas unicas).
 */
static void vmap_insert_fresh(VMapGroup *groups, uint64_t group_cnt,
                              uint64_t   key, uint64_t    value) {
    const uint64_t mask = group_cnt - 1;
    const uint64_t hash = wyhash64_u64(key);
    const uint8_t  h2   = vmap_h2(hash);
    uint64_t       g    = vmap_h1(hash) & mask;
    /* probing triangular: paso k = 0, 1, 3, 6, 10, ... */
    for (uint64_t step = 1; ; ++step) {
        VMapGroup *grp     = &groups[g];
        uint32_t   empties = vmap_match_ctrl(grp->ctrl, VMAP_CTRL_EMPTY);
        if (empties) {
            /* primer slot empty del grupo (bit menos significativo) */
            int bit        = __builtin_ctz(empties);
            grp->ctrl[bit] = h2;
            grp->keys[bit] = key;
            grp->vals[bit] = value;
            return;
        }
        g = (g + step) & mask;
    }
}

/**
 * @brief Rehash a una nueva tabla del doble de grupos.
 * @return 1 si OK, 0 si malloc fallo (la tabla queda con la version vieja).
 */
static int vmap_rehash(VestaHashMap *m) {
    const uint64_t new_group_cnt = m->group_cnt * 2;
    VMapGroup *    new_groups    = (VMapGroup *) malloc(new_group_cnt * sizeof(VMapGroup));
    if (!new_groups) return 0;
    /* inicializar todos los control bytes a EMPTY (bit 7 alto). */
    memset(new_groups, VMAP_CTRL_EMPTY,
           new_group_cnt * sizeof(VMapGroup));
    /* nota: el memset llena keys/vals con basura tambien, pero no se leen
       hasta que el ctrl correspondiente sea valido (h2). */
    /* reinsertar entradas validas (skip empty + deleted) */
    for (uint64_t g = 0; g < m->group_cnt; ++g) {
        VMapGroup *grp = &m->groups[g];
        for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
            uint8_t c = grp->ctrl[i];
            if ((c & 0x80) == 0) {
                /* h2 valido (bit 7 = 0) */
                vmap_insert_fresh(new_groups, new_group_cnt,
                                  grp->keys[i], grp->vals[i]);
            }
        }
    }
    free(m->groups);
    m->groups    = new_groups;
    m->group_cnt = new_group_cnt;
    /* threshold = 87.5% = 14/16 por grupo */
    m->threshold = (vmap_capacity(m) * 7u) / 8u;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_new(uint64_t initial_capacity) {
    /* Convertir capacidad solicitada a numero de grupos (cada uno 16 slots).
       Minimo 1 grupo (16 slots). */
    uint64_t cap = initial_capacity < VMAP_MIN_CAP
                       ? VMAP_MIN_CAP
                       : initial_capacity;
    cap                = next_pow2(cap);
    uint64_t group_cnt = cap / VMAP_GROUP_W;
    if (group_cnt < 1) group_cnt = 1;
    VestaHashMap *m = (VestaHashMap *) malloc(sizeof(VestaHashMap));
    if (!m) return VCOL_NULL;
    m->groups = (VMapGroup *) malloc(group_cnt * sizeof(VMapGroup));
    if (!m->groups) {
        free(m);
        return VCOL_NULL;
    }
    /* todos los slots empiezan EMPTY */
    memset(m->groups, VMAP_CTRL_EMPTY, group_cnt * sizeof(VMapGroup));
    m->group_cnt = group_cnt;
    m->size      = 0;
    m->threshold = (group_cnt * VMAP_GROUP_W * 7u) / 8u;
    return (uint64_t) (uintptr_t) m;
}

VESTA_PLUGIN_EXPORT void vcol_map_free(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return;
    free(m->groups);
    free(m);
}

/**
 * @brief Helper: busca key en la tabla; devuelve grupo + slot si existe, o
 *        deja todo en -1 si no encontrada.  Ademas reporta el primer slot
 *        deleted/empty visto para reusar en put.  Probing triangular.
 *
 * @param m         Tabla.
 * @param key       Clave a buscar.
 * @param hash_out  (out) Hash precomputado de key.
 * @param g_out     (out) Indice de grupo donde se encontro la key, o
 *                  el primer grupo donde un EMPTY la habria terminado.
 * @param i_out     (out) Indice del slot dentro del grupo donde esta o
 *                  donde insertar.
 * @return 1 si key encontrada exactamente, 0 si no encontrada (entonces
 *         g_out/i_out apuntan al slot donde insertar).
 */
static int vmap_find_or_insert_slot(VestaHashMap *m, uint64_t      key,
                                    uint64_t *    g_out, uint32_t *i_out) {
    const uint64_t mask = m->group_cnt - 1;
    const uint64_t hash = wyhash64_u64(key);
    const uint8_t  h2   = vmap_h2(hash);
    uint64_t       g    = vmap_h1(hash) & mask;
    /* memoria del primer hueco (deleted o empty) visto durante el probing */
    uint64_t free_g = UINT64_MAX;
    uint32_t free_i = 0;
    for (uint64_t step = 1; ; ++step) {
        VMapGroup *grp = &m->groups[g];
        /* 1. matches por h2: scan SIMD, cada bit es un slot candidato */
        uint32_t hits = vmap_match_ctrl(grp->ctrl, h2);
        while (hits) {
            int bit = __builtin_ctz(hits);
            if (grp->keys[bit] == key) {
                *g_out = g;
                *i_out = (uint32_t) bit;
                return 1;
            }
            hits &= hits - 1; /* limpiar bit menos significativo */
        }
        /* 2. registrar el primer DELETED libre para potencial insercion */
        if (free_g == UINT64_MAX) {
            uint32_t dels = vmap_match_ctrl(grp->ctrl, VMAP_CTRL_DELETED);
            if (dels) {
                free_g = g;
                free_i = (uint32_t) __builtin_ctz(dels);
            }
        }
        /* 3. si hay EMPTY en el grupo, terminamos: la key no existe.  Reusar
           el deleted antes encontrado, o el primer empty de este grupo. */
        uint32_t empties = vmap_match_ctrl(grp->ctrl, VMAP_CTRL_EMPTY);
        if (empties) {
            if (free_g != UINT64_MAX) {
                *g_out = free_g;
                *i_out = free_i;
            } else {
                *g_out = g;
                *i_out = (uint32_t) __builtin_ctz(empties);
            }
            return 0;
        }
        /* 4. avanzar al siguiente grupo (paso triangular) */
        g = (g + step) & mask;
    }
}

VESTA_PLUGIN_EXPORT void vcol_map_put(uint64_t handle, uint64_t key, uint64_t value) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return;
    /* rehash si vamos a superar el threshold (87.5% load) */
    if (m->size + 1 > m->threshold) {
        if (!vmap_rehash(m)) return; /* malloc fallo: silently drop */
    }
    uint64_t   g;
    uint32_t   i;
    int        found = vmap_find_or_insert_slot(m, key, &g, &i);
    VMapGroup *grp   = &m->groups[g];
    if (found) {
        grp->vals[i] = value; /* update */
    } else {
        const uint64_t hash = wyhash64_u64(key);
        grp->ctrl[i]        = vmap_h2(hash);
        grp->keys[i]        = key;
        grp->vals[i]        = value;
        m->size++;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_get(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return 0;
    uint64_t g;
    uint32_t i;
    if (vmap_find_or_insert_slot(m, key, &g, &i)) {
        return m->groups[g].vals[i];
    }
    return 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_get_or(uint64_t handle, uint64_t key,
                                             uint64_t default_val) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return default_val;
    uint64_t g;
    uint32_t i;
    if (vmap_find_or_insert_slot(m, key, &g, &i)) {
        return m->groups[g].vals[i];
    }
    return default_val;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_contains(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return 0;
    uint64_t g;
    uint32_t i;
    return vmap_find_or_insert_slot(m, key, &g, &i) ? 1u : 0u;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_remove(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return VCOL_NOT_FOUND;
    uint64_t g;
    uint32_t i;
    if (!vmap_find_or_insert_slot(m, key, &g, &i)) return VCOL_NOT_FOUND;
    VMapGroup *grp = &m->groups[g];
    uint64_t   old = grp->vals[i];
    /* Si el grupo tiene algun EMPTY, podemos borrar como EMPTY (no rompe
       cadena de probing porque cualquier lookup termina aqui de todos modos).
       Si el grupo esta lleno, marcamos como DELETED (tombstone) para
       preservar la cadena. */
    if (vmap_match_ctrl(grp->ctrl, VMAP_CTRL_EMPTY)) {
        grp->ctrl[i] = VMAP_CTRL_EMPTY;
    } else {
        grp->ctrl[i] = VMAP_CTRL_DELETED;
    }
    m->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_size(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    return m ? m->size : 0;
}

VESTA_PLUGIN_EXPORT void vcol_map_clear(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return;
    memset(m->groups, VMAP_CTRL_EMPTY, m->group_cnt * sizeof(VMapGroup));
    m->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_keys(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return VCOL_NULL;
    uint64_t al = vcol_alist_new(m->size > 0 ? m->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    for (uint64_t g = 0; g < m->group_cnt; ++g) {
        VMapGroup *grp = &m->groups[g];
        for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
            if ((grp->ctrl[i] & 0x80) == 0) {
                /* slot ocupado */
                vcol_alist_push(al, grp->keys[i]);
            }
        }
    }
    return al;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_values(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (!m) return VCOL_NULL;
    uint64_t al = vcol_alist_new(m->size > 0 ? m->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    for (uint64_t g = 0; g < m->group_cnt; ++g) {
        VMapGroup *grp = &m->groups[g];
        for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
            if ((grp->ctrl[i] & 0x80) == 0) {
                vcol_alist_push(al, grp->vals[i]);
            }
        }
    }
    return al;
}

/* =========================================================================
 * VestaHashSet -- envoltura sobre VestaHashMap
 * ========================================================================= */

/** @brief Valor internal sentinel para indicar "elemento presente" en el set. */
#define VSET_PRESENT (UINT64_MAX - 2u)

VESTA_PLUGIN_EXPORT uint64_t vcol_set_new(uint64_t initial_capacity) {
    return vcol_map_new(initial_capacity); /* reutilizar HashMap internamente */
}

VESTA_PLUGIN_EXPORT void vcol_set_free(uint64_t handle) {
    vcol_map_free(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_add(uint64_t handle, uint64_t element) {
    if (vcol_map_contains(handle, element)) return 0; /* ya existia */
    vcol_map_put(handle, element, VSET_PRESENT);
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_contains(uint64_t handle, uint64_t element) {
    return vcol_map_contains(handle, element);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_remove(uint64_t handle, uint64_t element) {
    return vcol_map_remove(handle, element) != VCOL_NOT_FOUND ? 1 : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_size(uint64_t handle) {
    return vcol_map_size(handle);
}

VESTA_PLUGIN_EXPORT void vcol_set_clear(uint64_t handle) {
    vcol_map_clear(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_to_list(uint64_t handle) {
    return vcol_map_keys(handle); /* las claves son los elementos del set */
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_is_subset(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a = (VestaHashMap *) (uintptr_t) set_a;
    if (!a) return 1; /* conjunto vacio es subconjunto de cualquiera */
    for (uint64_t g = 0; g < a->group_cnt; ++g) {
        VMapGroup *grp = &a->groups[g];
        for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
            if ((grp->ctrl[i] & 0x80) != 0) continue; /* skip empty/deleted */
            if (!vcol_map_contains(set_b, grp->keys[i])) return 0;
        }
    }
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_union(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a      = (VestaHashMap *) (uintptr_t) set_a;
    VestaHashMap *b      = (VestaHashMap *) (uintptr_t) set_b;
    uint64_t      sz     = (a ? a->size : 0) + (b ? b->size : 0);
    uint64_t      result = vcol_set_new(sz > VMAP_MIN_CAP ? sz : VMAP_MIN_CAP);
    if (!result) return VCOL_NULL;
    if (a) {
        for (uint64_t g = 0; g < a->group_cnt; ++g) {
            VMapGroup *grp = &a->groups[g];
            for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
                if ((grp->ctrl[i] & 0x80) == 0) {
                    vcol_map_put(result, grp->keys[i], VSET_PRESENT);
                }
            }
        }
    }
    if (b) {
        for (uint64_t g = 0; g < b->group_cnt; ++g) {
            VMapGroup *grp = &b->groups[g];
            for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
                if ((grp->ctrl[i] & 0x80) == 0) {
                    vcol_map_put(result, grp->keys[i], VSET_PRESENT);
                }
            }
        }
    }
    return result;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_intersect(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a      = (VestaHashMap *) (uintptr_t) set_a;
    uint64_t      sz     = a ? a->size : 0;
    uint64_t      result = vcol_set_new(sz > VMAP_MIN_CAP ? sz : VMAP_MIN_CAP);
    if (!result) return VCOL_NULL;
    if (!a) return result;
    for (uint64_t g = 0; g < a->group_cnt; ++g) {
        VMapGroup *grp = &a->groups[g];
        for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
            if ((grp->ctrl[i] & 0x80) != 0) continue;
            if (vcol_map_contains(set_b, grp->keys[i])) {
                vcol_map_put(result, grp->keys[i], VSET_PRESENT);
            }
        }
    }
    return result;
}

/* =========================================================================
 * VestaQueue -- ring buffer FIFO de uint64_t
 *
 * Estructura: array circular de capacidad potencia de 2.  head = indice
 * del proximo a leer (pop), tail = indice del proximo slot libre (push).
 * mask = cap-1 para wrap-around sin modulo.
 * size = (tail - head) & mask cuando tail < head (wrap), o tail - head normal.
 * Lleno: size == cap (cuando size++ == cap antes del push, hay que crecer).
 *
 * Crece x2 cuando lleno, copiando los elementos para que head=0 en la
 * nueva capacidad.  Coste amortizado: O(1) por push.
 * ========================================================================= */

#define VQUEUE_MIN_CAP 16u

typedef struct VestaQueue {
    uint64_t *data; /* array circular de capacidad cap */
    uint64_t  cap;  /* capacidad (potencia de 2) */
    uint64_t  mask; /* cap - 1 */
    uint64_t  head; /* indice del proximo a popear */
    uint64_t  tail; /* indice del proximo slot libre para push */
    uint64_t  size; /* numero de elementos actuales */
} VestaQueue;

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_new(uint64_t initial_capacity) {
    uint64_t cap = next_pow2(initial_capacity < VQUEUE_MIN_CAP
                                 ? VQUEUE_MIN_CAP
                                 : initial_capacity);
    VestaQueue *q = (VestaQueue *) malloc(sizeof(VestaQueue));
    if (!q) return VCOL_NULL;
    q->data = (uint64_t *) malloc(cap * sizeof(uint64_t));
    if (!q->data) {
        free(q);
        return VCOL_NULL;
    }
    q->cap  = cap;
    q->mask = cap - 1;
    q->head = 0;
    q->tail = 0;
    q->size = 0;
    return (uint64_t) (uintptr_t) q;
}

VESTA_PLUGIN_EXPORT void vcol_queue_free(uint64_t handle) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (!q) return;
    free(q->data);
    free(q);
}

/**
 * @brief Crece la capacidad x2 copiando elementos a partir de head=0.
 */
static int vqueue_grow(VestaQueue *q) {
    uint64_t  new_cap  = q->cap * 2;
    uint64_t *new_data = (uint64_t *) malloc(new_cap * sizeof(uint64_t));
    if (!new_data) return 0;
    /* Copia "linealizada": head se mueve al inicio del buffer nuevo.
       Si el ring no esta wrapped (head < tail): un solo memcpy.
       Si esta wrapped: dos memcpys (head..cap-1) + (0..tail-1). */
    if (q->head < q->tail) {
        memcpy(new_data, q->data + q->head, (q->tail - q->head) * sizeof(uint64_t));
    } else {
        size_t first = q->cap - q->head;
        memcpy(new_data, q->data + q->head, first * sizeof(uint64_t));
        if (q->tail > 0) {
            memcpy(new_data + first, q->data, q->tail * sizeof(uint64_t));
        }
    }
    free(q->data);
    q->data = new_data;
    q->cap  = new_cap;
    q->mask = new_cap - 1;
    q->head = 0;
    q->tail = q->size;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_push(uint64_t handle, uint64_t value) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (!q) return VCOL_NOT_FOUND;
    if (q->size == q->cap) {
        if (!vqueue_grow(q)) return VCOL_NOT_FOUND;
    }
    q->data[q->tail] = value;
    q->tail          = (q->tail + 1) & q->mask;
    q->size++;
    return q->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_pop(uint64_t handle) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (!q || q->size == 0) return VCOL_NOT_FOUND;
    uint64_t v = q->data[q->head];
    q->head    = (q->head + 1) & q->mask;
    q->size--;
    return v;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_peek(uint64_t handle) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (!q || q->size == 0) return VCOL_NOT_FOUND;
    return q->data[q->head];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_size(uint64_t handle) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    return q ? q->size : 0;
}

VESTA_PLUGIN_EXPORT void vcol_queue_clear(uint64_t handle) {
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (!q) return;
    q->head = 0;
    q->tail = 0;
    q->size = 0;
}

/* =========================================================================
 * VestaDeque -- ring buffer doble (push/pop ambos extremos)
 *
 * Mismo layout que Queue pero con push_front (decrementa head) y
 * pop_back (decrementa tail).  Wrap-around: head = (head - 1) & mask
 * funciona correctamente con potencia de 2 (mascara binaria).
 * ========================================================================= */

typedef struct VestaDeque {
    uint64_t *data;
    uint64_t  cap;
    uint64_t  mask;
    uint64_t  head;
    uint64_t  tail;
    uint64_t  size;
} VestaDeque;

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_new(uint64_t initial_capacity) {
    /* Reusa la implementacion de Queue (mismo layout). */
    return vcol_queue_new(initial_capacity);
}

VESTA_PLUGIN_EXPORT void vcol_deque_free(uint64_t handle) {
    vcol_queue_free(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_back(uint64_t handle, uint64_t value) {
    return vcol_queue_push(handle, value);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_front(uint64_t handle, uint64_t value) {
    VestaDeque *d = (VestaDeque *) (uintptr_t) handle;
    if (!d) return VCOL_NOT_FOUND;
    if (d->size == d->cap) {
        if (!vqueue_grow((VestaQueue *) d)) return VCOL_NOT_FOUND;
    }
    d->head          = (d->head - 1) & d->mask;
    d->data[d->head] = value;
    d->size++;
    return d->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_front(uint64_t handle) {
    return vcol_queue_pop(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_back(uint64_t handle) {
    VestaDeque *d = (VestaDeque *) (uintptr_t) handle;
    if (!d || d->size == 0) return VCOL_NOT_FOUND;
    d->tail    = (d->tail - 1) & d->mask;
    uint64_t v = d->data[d->tail];
    d->size--;
    return v;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_peek_front(uint64_t handle) {
    return vcol_queue_peek(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_peek_back(uint64_t handle) {
    VestaDeque *d = (VestaDeque *) (uintptr_t) handle;
    if (!d || d->size == 0) return VCOL_NOT_FOUND;
    return d->data[(d->tail - 1) & d->mask];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_size(uint64_t handle) {
    return vcol_queue_size(handle);
}

VESTA_PLUGIN_EXPORT void vcol_deque_clear(uint64_t handle) {
    vcol_queue_clear(handle);
}

/* =========================================================================
 * VestaTreeMap / VestaTreeSet -- arbol Red-Black uint64_t -> uint64_t
 *
 * Implementacion Red-Black clasica con sentinela NIL compartido.  Reglas:
 *   1. Cada nodo es ROJO o NEGRO.
 *   2. Raiz y sentinela NIL son NEGROS.
 *   3. Hijos de un nodo ROJO son NEGROS.
 *   4. Cada camino de la raiz a una hoja NIL tiene mismo numero de nodos NEGROS.
 *
 * Garantia: altura del arbol <= 2 * log2(n+1), todas las ops O(log n).
 * Comparacion de claves: numerica uint64_t (caller proporciona la
 * representacion comparable).
 * ========================================================================= */

typedef enum { VTREE_BLACK = 0, VTREE_RED = 1 } VTreeColor;

typedef struct VTreeNode {
    uint64_t          key;
    uint64_t          value;
    VTreeColor        color;
    struct VTreeNode *left;
    struct VTreeNode *right;
    struct VTreeNode *parent;
} VTreeNode;

typedef struct VestaTreeMap {
    VTreeNode *root;
    VTreeNode *nil; /* sentinela NIL compartido (NEGRO) */
    uint64_t   size;
} VestaTreeMap;

/* === Utilidades NIL/asignacion === */

static VTreeNode *vtree_make_node(VestaTreeMap *t, uint64_t key, uint64_t value) {
    VTreeNode *n = (VTreeNode *) malloc(sizeof(VTreeNode));
    if (!n) return NULL;
    n->key    = key;
    n->value  = value;
    n->color  = VTREE_RED; /* nodos nuevos siempre RED, balanceo lo arregla */
    n->left   = t->nil;
    n->right  = t->nil;
    n->parent = t->nil;
    return n;
}

static void vtree_free_node(VTreeNode *n, VTreeNode *nil) {
    if (n == nil) return;
    vtree_free_node(n->left, nil);
    vtree_free_node(n->right, nil);
    free(n);
}

/* === Rotaciones Red-Black === */

static void vtree_left_rotate(VestaTreeMap *t, VTreeNode *x) {
    VTreeNode *y = x->right;
    x->right     = y->left;
    if (y->left != t->nil) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == t->nil) t->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right                          = y;
    y->left   = x;
    x->parent = y;
}

static void vtree_right_rotate(VestaTreeMap *t, VTreeNode *x) {
    VTreeNode *y = x->left;
    x->left      = y->right;
    if (y->right != t->nil) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == t->nil) t->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left                             = y;
    y->right  = x;
    x->parent = y;
}

/* === Insercion fix-up (Cormen capitulo 13) === */

static void vtree_insert_fixup(VestaTreeMap *t, VTreeNode *z) {
    while (z->parent->color == VTREE_RED) {
        if (z->parent == z->parent->parent->left) {
            VTreeNode *y = z->parent->parent->right; /* tio */
            if (y->color == VTREE_RED) {
                z->parent->color         = VTREE_BLACK;
                y->color                 = VTREE_BLACK;
                z->parent->parent->color = VTREE_RED;
                z                        = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    vtree_left_rotate(t, z);
                }
                z->parent->color         = VTREE_BLACK;
                z->parent->parent->color = VTREE_RED;
                vtree_right_rotate(t, z->parent->parent);
            }
        } else {
            VTreeNode *y = z->parent->parent->left;
            if (y->color == VTREE_RED) {
                z->parent->color         = VTREE_BLACK;
                y->color                 = VTREE_BLACK;
                z->parent->parent->color = VTREE_RED;
                z                        = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    vtree_right_rotate(t, z);
                }
                z->parent->color         = VTREE_BLACK;
                z->parent->parent->color = VTREE_RED;
                vtree_left_rotate(t, z->parent->parent);
            }
        }
    }
    t->root->color = VTREE_BLACK;
}

/* === Eliminacion (Cormen capitulo 13) === */

static void vtree_transplant(VestaTreeMap *t, VTreeNode *u, VTreeNode *v) {
    if (u->parent == t->nil) t->root = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right                          = v;
    v->parent = u->parent;
}

static VTreeNode *vtree_minimum(VestaTreeMap *t, VTreeNode *x) {
    while (x->left != t->nil) x = x->left;
    return x;
}

static void vtree_delete_fixup(VestaTreeMap *t, VTreeNode *x) {
    while (x != t->root && x->color == VTREE_BLACK) {
        if (x == x->parent->left) {
            VTreeNode *w = x->parent->right;
            if (w->color == VTREE_RED) {
                w->color         = VTREE_BLACK;
                x->parent->color = VTREE_RED;
                vtree_left_rotate(t, x->parent);
                w = x->parent->right;
            }
            if (w->left->color == VTREE_BLACK && w->right->color == VTREE_BLACK) {
                w->color = VTREE_RED;
                x        = x->parent;
            } else {
                if (w->right->color == VTREE_BLACK) {
                    w->left->color = VTREE_BLACK;
                    w->color       = VTREE_RED;
                    vtree_right_rotate(t, w);
                    w = x->parent->right;
                }
                w->color         = x->parent->color;
                x->parent->color = VTREE_BLACK;
                w->right->color  = VTREE_BLACK;
                vtree_left_rotate(t, x->parent);
                x = t->root;
            }
        } else {
            VTreeNode *w = x->parent->left;
            if (w->color == VTREE_RED) {
                w->color         = VTREE_BLACK;
                x->parent->color = VTREE_RED;
                vtree_right_rotate(t, x->parent);
                w = x->parent->left;
            }
            if (w->right->color == VTREE_BLACK && w->left->color == VTREE_BLACK) {
                w->color = VTREE_RED;
                x        = x->parent;
            } else {
                if (w->left->color == VTREE_BLACK) {
                    w->right->color = VTREE_BLACK;
                    w->color        = VTREE_RED;
                    vtree_left_rotate(t, w);
                    w = x->parent->left;
                }
                w->color         = x->parent->color;
                x->parent->color = VTREE_BLACK;
                w->left->color   = VTREE_BLACK;
                vtree_right_rotate(t, x->parent);
                x = t->root;
            }
        }
    }
    x->color = VTREE_BLACK;
}

/* === Busqueda interna === */

static VTreeNode *vtree_find(VestaTreeMap *t, uint64_t key) {
    VTreeNode *cur = t->root;
    while (cur != t->nil) {
        if (key == cur->key) return cur;
        cur = (key < cur->key) ? cur->left : cur->right;
    }
    return NULL;
}

/* === API publica TreeMap === */

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_new(void) {
    VestaTreeMap *t = (VestaTreeMap *) malloc(sizeof(VestaTreeMap));
    if (!t) return VCOL_NULL;
    t->nil = (VTreeNode *) malloc(sizeof(VTreeNode));
    if (!t->nil) {
        free(t);
        return VCOL_NULL;
    }
    t->nil->color  = VTREE_BLACK;
    t->nil->left   = t->nil;
    t->nil->right  = t->nil;
    t->nil->parent = t->nil;
    t->nil->key    = 0;
    t->nil->value  = 0;
    t->root        = t->nil;
    t->size        = 0;
    return (uint64_t) (uintptr_t) t;
}

VESTA_PLUGIN_EXPORT void vcol_tmap_free(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return;
    vtree_free_node(t->root, t->nil);
    free(t->nil);
    free(t);
}

VESTA_PLUGIN_EXPORT void vcol_tmap_put(uint64_t handle, uint64_t key, uint64_t value) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return;
    /* Si la key existe, update y salir. */
    VTreeNode *exist = vtree_find(t, key);
    if (exist) {
        exist->value = value;
        return;
    }
    /* Insercion BST estandar + fixup */
    VTreeNode *z = vtree_make_node(t, key, value);
    if (!z) return;
    VTreeNode *y = t->nil;
    VTreeNode *x = t->root;
    while (x != t->nil) {
        y = x;
        x = (z->key < x->key) ? x->left : x->right;
    }
    z->parent = y;
    if (y == t->nil) t->root = z;
    else if (z->key < y->key) y->left = z;
    else y->right                     = z;
    vtree_insert_fixup(t, z);
    t->size++;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_get(uint64_t handle, uint64_t key) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return 0;
    VTreeNode *n = vtree_find(t, key);
    return n ? n->value : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_contains(uint64_t handle, uint64_t key) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return 0;
    return vtree_find(t, key) ? 1u : 0u;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_remove(uint64_t handle, uint64_t key) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return VCOL_NOT_FOUND;
    VTreeNode *z = vtree_find(t, key);
    if (!z) return VCOL_NOT_FOUND;
    uint64_t   old    = z->value;
    VTreeNode *y      = z;
    VTreeColor y_orig = y->color;
    VTreeNode *x;
    if (z->left == t->nil) {
        x = z->right;
        vtree_transplant(t, z, z->right);
    } else if (z->right == t->nil) {
        x = z->left;
        vtree_transplant(t, z, z->left);
    } else {
        y      = vtree_minimum(t, z->right);
        y_orig = y->color;
        x      = y->right;
        if (y->parent == z) {
            x->parent = y;
        } else {
            vtree_transplant(t, y, y->right);
            y->right         = z->right;
            y->right->parent = y;
        }
        vtree_transplant(t, z, y);
        y->left         = z->left;
        y->left->parent = y;
        y->color        = z->color;
    }
    free(z);
    if (y_orig == VTREE_BLACK) vtree_delete_fixup(t, x);
    t->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_size(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    return t ? t->size : 0;
}

VESTA_PLUGIN_EXPORT void vcol_tmap_clear(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return;
    vtree_free_node(t->root, t->nil);
    t->root = t->nil;
    t->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_first_key(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t || t->root == t->nil) return VCOL_NOT_FOUND;
    return vtree_minimum(t, t->root)->key;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_last_key(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t || t->root == t->nil) return VCOL_NOT_FOUND;
    VTreeNode *x = t->root;
    while (x->right != t->nil) x = x->right;
    return x->key;
}

/* mayor key <= key dado.  O(log n).  Devuelve VCOL_NOT_FOUND si no existe. */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_floor_key(uint64_t handle, uint64_t key) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return VCOL_NOT_FOUND;
    VTreeNode *cur  = t->root;
    VTreeNode *best = NULL;
    while (cur != t->nil) {
        if (cur->key == key) return cur->key;
        if (cur->key < key) {
            best = cur;
            cur  = cur->right;
        } else {
            cur = cur->left;
        }
    }
    return best ? best->key : VCOL_NOT_FOUND;
}

/* menor key >= key dado.  O(log n). */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_ceiling_key(uint64_t handle, uint64_t key) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return VCOL_NOT_FOUND;
    VTreeNode *cur  = t->root;
    VTreeNode *best = NULL;
    while (cur != t->nil) {
        if (cur->key == key) return cur->key;
        if (cur->key > key) {
            best = cur;
            cur  = cur->left;
        } else {
            cur = cur->right;
        }
    }
    return best ? best->key : VCOL_NOT_FOUND;
}

/* Recorrido in-order recursivo a un alist (helper). */
static void vtree_inorder_keys(VTreeNode *n, VTreeNode *nil, uint64_t alist) {
    if (n == nil) return;
    vtree_inorder_keys(n->left, nil, alist);
    vcol_alist_push(alist, n->key);
    vtree_inorder_keys(n->right, nil, alist);
}

static void vtree_inorder_values(VTreeNode *n, VTreeNode *nil, uint64_t alist) {
    if (n == nil) return;
    vtree_inorder_values(n->left, nil, alist);
    vcol_alist_push(alist, n->value);
    vtree_inorder_values(n->right, nil, alist);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_keys(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return VCOL_NULL;
    uint64_t al = vcol_alist_new(t->size > 0 ? t->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    vtree_inorder_keys(t->root, t->nil, al);
    return al;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_values(uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (!t) return VCOL_NULL;
    uint64_t al = vcol_alist_new(t->size > 0 ? t->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    vtree_inorder_values(t->root, t->nil, al);
    return al;
}

/* === API publica TreeSet (wrapper sobre TreeMap con value=PRESENT) === */

#define VTSET_PRESENT (UINT64_MAX - 3u)

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_new(void) {
    return vcol_tmap_new();
}

VESTA_PLUGIN_EXPORT void vcol_tset_free(uint64_t h) {
    vcol_tmap_free(h);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_add(uint64_t h, uint64_t e) {
    if (vcol_tmap_contains(h, e)) return 0;
    vcol_tmap_put(h, e, VTSET_PRESENT);
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_contains(uint64_t h, uint64_t e) {
    return vcol_tmap_contains(h, e);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_remove(uint64_t h, uint64_t e) {
    return vcol_tmap_remove(h, e) != VCOL_NOT_FOUND ? 1u : 0u;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_size(uint64_t h) {
    return vcol_tmap_size(h);
}

VESTA_PLUGIN_EXPORT void vcol_tset_clear(uint64_t h) {
    vcol_tmap_clear(h);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_first(uint64_t h) {
    return vcol_tmap_first_key(h);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_last(uint64_t h) {
    return vcol_tmap_last_key(h);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_to_list(uint64_t h) {
    return vcol_tmap_keys(h);
}

/* =========================================================================
 * String ops nativas
 *
 * Operan sobre buffers crudos (host_ptr + byte_len).  Reusa las primitivas
 * libc memmem / memchr / memcmp que en glibc / msvcrt usan SIMD interno
 * (SSE2/AVX2 en x86-64) cuando el tamano lo amerita.  Cero re-implementacion.
 *
 * memmem es una extension GNU/BSD; en Windows MinGW no esta disponible
 * por defecto, asi que damos una implementacion fallback (Boyer-Moore-Horspool
 * lite) para portabilidad.
 * ========================================================================= */

/**
 * @brief Implementacion portable de memmem (busqueda de subcadena).
 *
 * Algoritmo: para needle_len <= 1 reduce a memchr.  Para needle pequeno
 * (<=4 bytes) usa busqueda naive con early-exit en primer byte (memchr).
 * Para needle largo usa Boyer-Moore-Horspool con tabla de skips de 256
 * bytes en stack -- subcuadratico en el caso esperado.
 */
static const void *vstr_memmem(const void *haystack, size_t hlen,
                               const void *needle, size_t   nlen) {
    if (nlen == 0) return haystack;
    if (hlen < nlen) return NULL;
    const unsigned char *h = (const unsigned char *) haystack;
    const unsigned char *n = (const unsigned char *) needle;
    if (nlen == 1) {
        return memchr(h, n[0], hlen);
    }
    if (nlen <= 4 || hlen <= 32) {
        /* Naive con memchr para acelerar el primer byte. */
        const unsigned char  first      = n[0];
        const unsigned char *end_search = h + (hlen - nlen) + 1;
        while (h < end_search) {
            const unsigned char *p = (const unsigned char *)
                    memchr(h, first, (size_t) (end_search - h));
            if (!p) return NULL;
            if (memcmp(p, n, nlen) == 0) return p;
            h = p + 1;
        }
        return NULL;
    }
    /* Boyer-Moore-Horspool simplificado.  Tabla de skip por byte. */
    size_t skip[256];
    for (size_t i = 0; i < 256; ++i) skip[i] = nlen;
    for (size_t i = 0; i + 1 < nlen; ++i) skip[n[i]] = nlen - 1 - i;
    size_t i = 0;
    while (i + nlen <= hlen) {
        if (memcmp(h + i, n, nlen) == 0) return h + i;
        i += skip[h[i + nlen - 1]];
    }
    return NULL;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_indexof(uint64_t haystack_ptr, uint64_t haystack_len,
                                          uint64_t needle_ptr, uint64_t   needle_len) {
    const unsigned char *h = (const unsigned char *) (uintptr_t) haystack_ptr;
    const unsigned char *n = (const unsigned char *) (uintptr_t) needle_ptr;
    if (!h || !n) return VCOL_NOT_FOUND;
    const void *p = vstr_memmem(h, (size_t) haystack_len, n, (size_t) needle_len);
    return p ? (uint64_t) ((const unsigned char *) p - h) : VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_indexof_from(uint64_t haystack_ptr, uint64_t haystack_len,
                                               uint64_t needle_ptr, uint64_t   needle_len,
                                               uint64_t start) {
    if (start >= haystack_len) return VCOL_NOT_FOUND;
    const unsigned char *h = (const unsigned char *) (uintptr_t) haystack_ptr;
    const unsigned char *n = (const unsigned char *) (uintptr_t) needle_ptr;
    if (!h || !n) return VCOL_NOT_FOUND;
    const void *p = vstr_memmem(h + start, (size_t) (haystack_len - start),
                                n, (size_t) needle_len);
    return p ? (uint64_t) ((const unsigned char *) p - h) : VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_contains(uint64_t haystack_ptr, uint64_t haystack_len,
                                           uint64_t needle_ptr, uint64_t   needle_len) {
    return vstr_indexof(haystack_ptr, haystack_len, needle_ptr, needle_len)
           != VCOL_NOT_FOUND
               ? 1u
               : 0u;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_starts_with(uint64_t haystack_ptr, uint64_t haystack_len,
                                              uint64_t needle_ptr, uint64_t   needle_len) {
    if (needle_len > haystack_len) return 0;
    const void *h = (const void *) (uintptr_t) haystack_ptr;
    const void *n = (const void *) (uintptr_t) needle_ptr;
    if (!h || !n) return 0;
    return memcmp(h, n, (size_t) needle_len) == 0 ? 1u : 0u;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_ends_with(uint64_t haystack_ptr, uint64_t haystack_len,
                                            uint64_t needle_ptr, uint64_t   needle_len) {
    if (needle_len > haystack_len) return 0;
    const unsigned char *h = (const unsigned char *) (uintptr_t) haystack_ptr;
    const void *         n = (const void *) (uintptr_t) needle_ptr;
    if (!h || !n) return 0;
    return memcmp(h + (haystack_len - needle_len), n, (size_t) needle_len) == 0 ? 1u : 0u;
}

VESTA_PLUGIN_EXPORT void vstr_lower_inplace(uint64_t ptr, uint64_t len) {
    unsigned char *p = (unsigned char *) (uintptr_t) ptr;
    if (!p) return;
    for (uint64_t i = 0; i < len; ++i) {
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] = (unsigned char) (p[i] + 32);
    }
}

VESTA_PLUGIN_EXPORT void vstr_upper_inplace(uint64_t ptr, uint64_t len) {
    unsigned char *p = (unsigned char *) (uintptr_t) ptr;
    if (!p) return;
    for (uint64_t i = 0; i < len; ++i) {
        if (p[i] >= 'a' && p[i] <= 'z') p[i] = (unsigned char) (p[i] - 32);
    }
}

static int vstr_is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

VESTA_PLUGIN_EXPORT uint64_t vstr_trim_start_offset(uint64_t ptr, uint64_t len) {
    const unsigned char *p = (const unsigned char *) (uintptr_t) ptr;
    if (!p) return 0;
    uint64_t i = 0;
    while (i < len && vstr_is_ws(p[i])) ++i;
    return i;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_trim_end_strip(uint64_t ptr, uint64_t len) {
    const unsigned char *p = (const unsigned char *) (uintptr_t) ptr;
    if (!p) return 0;
    uint64_t strip = 0;
    while (strip < len && vstr_is_ws(p[len - 1 - strip])) ++strip;
    return strip;
}

VESTA_PLUGIN_EXPORT uint64_t vstr_split_offsets(uint64_t haystack_ptr, uint64_t haystack_len,
                                                uint64_t delim_ptr, uint64_t    delim_len) {
    const unsigned char *h = (const unsigned char *) (uintptr_t) haystack_ptr;
    const unsigned char *d = (const unsigned char *) (uintptr_t) delim_ptr;
    if (!h) return VCOL_NULL;
    uint64_t list = vcol_alist_new(16);
    if (!list) return VCOL_NULL;
    /* delim vacio: degeneraria a infinito; devolvemos [(0,len)] como
       un solo elemento que es el haystack entero. */
    if (delim_len == 0 || !d) {
        vcol_alist_push(list, ((uint64_t) 0 << 32) | (haystack_len & 0xFFFFFFFFu));
        return list;
    }
    uint64_t cur = 0;
    while (cur <= haystack_len) {
        const unsigned char *m;
        if (cur < haystack_len) {
            m = (const unsigned char *)
                    vstr_memmem(h + cur, (size_t) (haystack_len - cur),
                                d, (size_t) delim_len);
        } else {
            m = NULL;
        }
        if (!m) {
            /* ultima substring (puede estar vacia si haystack acaba en delim) */
            uint64_t off = cur;
            uint64_t len = haystack_len - cur;
            if (len > 0 || haystack_len == 0) {
                vcol_alist_push(list, (off << 32) | (len & 0xFFFFFFFFu));
            }
            break;
        }
        uint64_t off = cur;
        uint64_t len = (uint64_t) (m - (h + cur));
        /* Incluimos substrings vacias para semantica predecible
           (split("a,,b", ",") -> ["a", "", "b"]). */
        vcol_alist_push(list, (off << 32) | (len & 0xFFFFFFFFu));
        cur = off + len + delim_len;
    }
    return list;
}

/* =========================================================================
 * Array ops nativas
 * ========================================================================= */

static int varr_cmp_u64_asc(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *) a;
    uint64_t vb = *(const uint64_t *) b;
    return (va > vb) - (va < vb);
}

static int varr_cmp_u64_desc(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *) a;
    uint64_t vb = *(const uint64_t *) b;
    return (va < vb) - (va > vb);
}

VESTA_PLUGIN_EXPORT void varr_sort_u64(uint64_t ptr, uint64_t n) {
    uint64_t *p = (uint64_t *) (uintptr_t) ptr;
    if (!p || n < 2) return;
    /* qsort de la libc usa introsort/median-of-three en MinGW; no es el
       mas rapido posible (pdqsort seria 30-50% mejor) pero suficiente
       como baseline.  Migrable a pdqsort en el futuro sin cambiar la API. */
    qsort(p, (size_t) n, sizeof(uint64_t), varr_cmp_u64_asc);
}

VESTA_PLUGIN_EXPORT void varr_sort_u64_desc(uint64_t ptr, uint64_t n) {
    uint64_t *p = (uint64_t *) (uintptr_t) ptr;
    if (!p || n < 2) return;
    qsort(p, (size_t) n, sizeof(uint64_t), varr_cmp_u64_desc);
}

VESTA_PLUGIN_EXPORT uint64_t varr_bsearch_u64(uint64_t ptr, uint64_t n, uint64_t key) {
    const uint64_t *p = (const uint64_t *) (uintptr_t) ptr;
    if (!p || n == 0) return VCOL_NOT_FOUND;
    /* Branchless binary search: cmovs en lugar de jumps en el loop interno.
       En arrays >= 1K elementos suele dar 20-40% mejora sobre la version con
       branches por mejor pipeline utilization.  Para n pequeno la diferencia
       es despreciable. */
    uint64_t lo = 0, len = n;
    while (len > 0) {
        uint64_t half = len >> 1;
        uint64_t mid  = lo + half;
        /* La asignacion siguiente compila a cmov en gcc -O2/-O3 */
        lo  = (p[mid] < key) ? (mid + 1) : lo;
        len = (p[mid] < key) ? (len - half - 1) : half;
    }
    return (lo < n && p[lo] == key) ? lo : VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t varr_indexof_u64(uint64_t ptr, uint64_t n, uint64_t key) {
    const uint64_t *p = (const uint64_t *) (uintptr_t) ptr;
    if (!p) return VCOL_NOT_FOUND;
    /* Loop simple; gcc -O3 lo vectoriza con SSE2 _mm_cmpeq_epi64 cuando n
       es grande y conocido en compile-time, pero aqui n es runtime: queda
       como linear scan.  Suficiente para colecciones de <1K elementos. */
    for (uint64_t i = 0; i < n; ++i) {
        if (p[i] == key) return i;
    }
    return VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT void varr_reverse_u64(uint64_t ptr, uint64_t n) {
    uint64_t *p = (uint64_t *) (uintptr_t) ptr;
    if (!p || n < 2) return;
    uint64_t i = 0, j = n - 1;
    while (i < j) {
        uint64_t tmp = p[i];
        p[i]         = p[j];
        p[j]         = tmp;
        ++i;
        --j;
    }
}

/* =========================================================================
 * Variantes GC-aware -- write-barrier para colecciones que retienen
 * GcHandles.  Cada operacion de escritura llama a g_api->gc_addref antes de
 * almacenar el slot; cada operacion de lectura-destructiva (pop/remove) o
 * clear/free llama a g_api->gc_release antes de descartar el slot.  Coste
 * O(1) por slot (1 lookup + atomic-like increment en GcHeap).
 *
 * Para maxima reuso, las variantes gc_aware delegan a las funciones no-gc
 * y solo anyaden las llamadas gc_addref/gc_release alrededor.
 * ========================================================================= */

/* Helper interno para llamar g_api->gc_addref si el API esta wired. */
static inline void vcol_gc_addref(uint64_t proc, uint64_t h) {
    if (g_api && g_api->gc_addref && proc != 0 && h != 0) {
        g_api->gc_addref(proc, h);
    }
}

static inline void vcol_gc_release(uint64_t proc, uint64_t h) {
    if (g_api && g_api->gc_release && proc != 0 && h != 0) {
        g_api->gc_release(proc, h);
    }
}

/* ===== ArrayList GC-aware ===== */

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_push_gc(uint64_t proc, uint64_t handle, uint64_t value) {
    vcol_gc_addref(proc, value);
    return vcol_alist_push(handle, value);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_pop_gc(uint64_t proc, uint64_t handle) {
    uint64_t v = vcol_alist_pop(handle);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_set_gc(uint64_t proc, uint64_t handle, uint64_t index, uint64_t value) {
    uint64_t old = vcol_alist_get(handle, index);
    vcol_gc_addref(proc, value);
    uint64_t r = vcol_alist_set(handle, index, value);
    if (old != VCOL_NOT_FOUND) vcol_gc_release(proc, old);
    return r;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_remove_at_gc(uint64_t proc, uint64_t handle, uint64_t index) {
    uint64_t v = vcol_alist_remove_at(handle, index);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_insert_gc(uint64_t proc, uint64_t handle, uint64_t index, uint64_t value) {
    vcol_gc_addref(proc, value);
    return vcol_alist_insert(handle, index, value);
}

VESTA_PLUGIN_EXPORT void vcol_alist_clear_gc(uint64_t proc, uint64_t handle) {
    /* Liberar refs antes de clear.  Iteramos manualmente para no llamar
       size repetidamente. */
    VestaArrayList *al = (VestaArrayList *) (uintptr_t) handle;
    if (al) {
        for (uint64_t i = 0; i < al->size; ++i) {
            vcol_gc_release(proc, al->data[i]);
        }
    }
    vcol_alist_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_alist_free_gc(uint64_t proc, uint64_t handle) {
    /* Igual que clear pero ademas free. */
    vcol_alist_clear_gc(proc, handle);
    vcol_alist_free(handle);
}

/* ===== HashMap GC-aware (values) ===== */

VESTA_PLUGIN_EXPORT void vcol_map_put_gc(uint64_t proc, uint64_t handle, uint64_t key, uint64_t value) {
    /* Si la key ya existia, debemos liberar el value anterior (sera
       reemplazado).  Detectamos via contains+get. */
    uint64_t had = vcol_map_contains(handle, key);
    uint64_t old = had ? vcol_map_get(handle, key) : 0;
    vcol_gc_addref(proc, value);
    vcol_map_put(handle, key, value);
    if (had && old != 0) vcol_gc_release(proc, old);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_remove_gc(uint64_t proc, uint64_t handle, uint64_t key) {
    uint64_t v = vcol_map_remove(handle, key);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT void vcol_map_clear_gc(uint64_t proc, uint64_t handle) {
    /* Iterar slots ocupados y liberar refs de los values. */
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (m) {
        for (uint64_t g = 0; g < m->group_cnt; ++g) {
            VMapGroup *grp = &m->groups[g];
            for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
                if ((grp->ctrl[i] & 0x80) == 0) {
                    vcol_gc_release(proc, grp->vals[i]);
                }
            }
        }
    }
    vcol_map_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_map_free_gc(uint64_t proc, uint64_t handle) {
    vcol_map_clear_gc(proc, handle);
    vcol_map_free(handle);
}

/* ===== HashSet GC-aware (elementos) ===== */

VESTA_PLUGIN_EXPORT uint64_t vcol_set_add_gc(uint64_t proc, uint64_t handle, uint64_t element) {
    /* set_add devuelve 0 si ya existia (no lo agrega); solo addref si
       efectivamente se agrego. */
    uint64_t had = vcol_set_contains(handle, element);
    uint64_t r   = vcol_set_add(handle, element);
    if (!had && r != 0) vcol_gc_addref(proc, element);
    return r;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_remove_gc(uint64_t proc, uint64_t handle, uint64_t element) {
    uint64_t r = vcol_set_remove(handle, element);
    if (r != 0) vcol_gc_release(proc, element);
    return r;
}

VESTA_PLUGIN_EXPORT void vcol_set_clear_gc(uint64_t proc, uint64_t handle) {
    /* Set es wrapper sobre HashMap; iterar igual. */
    VestaHashMap *m = (VestaHashMap *) (uintptr_t) handle;
    if (m) {
        for (uint64_t g = 0; g < m->group_cnt; ++g) {
            VMapGroup *grp = &m->groups[g];
            for (uint32_t i = 0; i < VMAP_GROUP_W; ++i) {
                if ((grp->ctrl[i] & 0x80) == 0) {
                    /* Para set, los keys SON los elementos (con value sentinel). */
                    vcol_gc_release(proc, grp->keys[i]);
                }
            }
        }
    }
    vcol_set_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_set_free_gc(uint64_t proc, uint64_t handle) {
    vcol_set_clear_gc(proc, handle);
    vcol_set_free(handle);
}

/* ===== Queue GC-aware ===== */

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_push_gc(uint64_t proc, uint64_t handle, uint64_t value) {
    vcol_gc_addref(proc, value);
    return vcol_queue_push(handle, value);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_pop_gc(uint64_t proc, uint64_t handle) {
    uint64_t v = vcol_queue_pop(handle);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT void vcol_queue_clear_gc(uint64_t proc, uint64_t handle) {
    /* Iterar elementos del ring buffer y liberar refs. */
    VestaQueue *q = (VestaQueue *) (uintptr_t) handle;
    if (q && q->size > 0) {
        uint64_t pos = q->head;
        for (uint64_t i = 0; i < q->size; ++i) {
            vcol_gc_release(proc, q->data[pos]);
            pos = (pos + 1) & q->mask;
        }
    }
    vcol_queue_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_queue_free_gc(uint64_t proc, uint64_t handle) {
    vcol_queue_clear_gc(proc, handle);
    vcol_queue_free(handle);
}

/* ===== Deque GC-aware ===== */

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_back_gc(uint64_t proc, uint64_t handle, uint64_t value) {
    vcol_gc_addref(proc, value);
    return vcol_deque_push_back(handle, value);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_front_gc(uint64_t proc, uint64_t handle, uint64_t value) {
    vcol_gc_addref(proc, value);
    return vcol_deque_push_front(handle, value);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_back_gc(uint64_t proc, uint64_t handle) {
    uint64_t v = vcol_deque_pop_back(handle);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_front_gc(uint64_t proc, uint64_t handle) {
    uint64_t v = vcol_deque_pop_front(handle);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT void vcol_deque_clear_gc(uint64_t proc, uint64_t handle) {
    vcol_queue_clear_gc(proc, handle); /* mismo layout */
}

VESTA_PLUGIN_EXPORT void vcol_deque_free_gc(uint64_t proc, uint64_t handle) {
    vcol_deque_clear_gc(proc, handle);
    vcol_deque_free(handle);
}

/* ===== TreeMap GC-aware ===== */

/* Helper: recorrido in-order para liberar values de un subtree. */
static void vtree_release_values(uint64_t proc, VTreeNode *n, VTreeNode *nil) {
    if (n == nil) return;
    vtree_release_values(proc, n->left, nil);
    vcol_gc_release(proc, n->value);
    vtree_release_values(proc, n->right, nil);
}

VESTA_PLUGIN_EXPORT void vcol_tmap_put_gc(uint64_t proc, uint64_t handle, uint64_t key, uint64_t value) {
    uint64_t had = vcol_tmap_contains(handle, key);
    uint64_t old = had ? vcol_tmap_get(handle, key) : 0;
    vcol_gc_addref(proc, value);
    vcol_tmap_put(handle, key, value);
    if (had && old != 0) vcol_gc_release(proc, old);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_remove_gc(uint64_t proc, uint64_t handle, uint64_t key) {
    uint64_t v = vcol_tmap_remove(handle, key);
    if (v != VCOL_NOT_FOUND) vcol_gc_release(proc, v);
    return v;
}

VESTA_PLUGIN_EXPORT void vcol_tmap_clear_gc(uint64_t proc, uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (t) vtree_release_values(proc, t->root, t->nil);
    vcol_tmap_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_tmap_free_gc(uint64_t proc, uint64_t handle) {
    vcol_tmap_clear_gc(proc, handle);
    vcol_tmap_free(handle);
}

/* ===== TreeSet GC-aware (elementos) ===== */

static void vtree_release_keys(uint64_t proc, VTreeNode *n, VTreeNode *nil) {
    if (n == nil) return;
    vtree_release_keys(proc, n->left, nil);
    vcol_gc_release(proc, n->key);
    vtree_release_keys(proc, n->right, nil);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_add_gc(uint64_t proc, uint64_t handle, uint64_t element) {
    uint64_t had = vcol_tset_contains(handle, element);
    uint64_t r   = vcol_tset_add(handle, element);
    if (!had && r != 0) vcol_gc_addref(proc, element);
    return r;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_remove_gc(uint64_t proc, uint64_t handle, uint64_t element) {
    uint64_t r = vcol_tset_remove(handle, element);
    if (r != 0) vcol_gc_release(proc, element);
    return r;
}

VESTA_PLUGIN_EXPORT void vcol_tset_clear_gc(uint64_t proc, uint64_t handle) {
    VestaTreeMap *t = (VestaTreeMap *) (uintptr_t) handle;
    if (t) vtree_release_keys(proc, t->root, t->nil);
    vcol_tset_clear(handle);
}

VESTA_PLUGIN_EXPORT void vcol_tset_free_gc(uint64_t proc, uint64_t handle) {
    vcol_tset_clear_gc(proc, handle);
    vcol_tset_free(handle);
}

/* =========================================================================
 * Punto de entrada del plugin
 * ========================================================================= */

/**
 * @brief Funcion de inicializacion llamada por VestaVM tras cargar el plugin.
 *
 * @param api Puntero a la tabla de funciones de la API de VestaVM.
 */
VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
#if VCOL_COLLECTIONS_DEBUG
    if (api) api->log("[vesta_collections] plugin cargado (debug ON)");
#else
    if (api) api->log("[vesta_collections] plugin cargado");
#endif
}
