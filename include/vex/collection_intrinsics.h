/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file collection_intrinsics.h
 * @brief Tabla declarativa que asocia los PrimitiveKind de coleccion
 *        a sus builtins de constructor / metodos / liberacion del plugin
 *        nativo @c vesta_collections.dll.
 *
 * El frontend Vex (type checker + lowering) consulta estas tablas para:
 *   1. Registrar el constructor (@c arraylist(), @c hashmap(), ...) como
 *      builtin que devuelve el tipo correspondiente.
 *   2. Despachar @c xs.method(...) a @c CALLN @c vcol_*_method directo
 *      (sin vtable, cero overhead).
 *   3. Emitir auto-free al exit del scope (cleanup_stack_) llamando a la
 *      funcion @c free correspondiente.
 *
 * Tipos primitivos soportados: ARRAYLIST, HASHMAP, HASHSET, QUEUE, DEQUE,
 * TREEMAP, TREESET, STACK.
 */
#pragma once

#include "vex/types.h"

#include <cstddef>

namespace vex {

    /** @brief Libreria nativa donde residen todos los simbolos de coleccion. */
    static constexpr const char *COL_NATIVE_LIB = "stdlib/native/collections/vesta_collections";

    /**
     * @struct ColType
     * @brief Mapeo de un PrimitiveKind de coleccion a sus funciones de
     *        construccion / liberacion + capacidad inicial sugerida.
     */
    struct ColType {
        PrimitiveKind  kind;            ///< PrimitiveKind del tipo Vex.
        const char    *vex_ctor_name;   ///< Nombre del constructor Vex (ej. "arraylist").
        const char    *native_new_fn;   ///< Funcion native (ej. "vcol_alist_new").
        const char    *native_free_fn;  ///< Funcion native de free (ej. "vcol_alist_free").
        /// Variante GC-aware del free.  Toma (proc, handle) en lugar de
        /// solo (handle): el plugin la usa para invocar @c gc_release
        /// sobre los slots que contienen GcHandles antes de liberar el
        /// almacenamiento.  nullptr si la coleccion no admite elementos
        /// GC (p.ej. ArrayList<i64> donde los elementos son escalares).
        const char    *native_free_fn_gc;
        int            default_cap;     ///< Capacidad por defecto (0 = ctor sin args).
    };

    /**
     * @struct ColMethod
     * @brief Metodo de coleccion: dispatch @c xs.method(args...) ->
     *        @c CALLN(this_handle, args...).
     *
     * Si la coleccion fue declarada con un tipo de elemento GC
     * (ej. @c ArrayList<string>) el lowering busca la variante
     * @c native_fn_gc en lugar de @c native_fn y emite un @c getproc
     * adicional como primer argumento.  La variante GC envuelve la
     * misma operacion pero llama @c gc_addref / @c gc_release del API
     * v2 para mantener vivos los handles que se quedan en el array.
     */
    struct ColMethod {
        PrimitiveKind   type;             ///< Tipo del receiver (this).
        const char     *vex_name;         ///< Nombre del metodo en Vex.
        const char     *native_fn;        ///< Funcion native (no-GC).
        /// Variante GC-aware (recibe @c proc como primer arg adicional);
        /// nullptr si esta operacion no necesita write-barrier.  Las
        /// operaciones puramente lectoras (size/get/contains/peek/...) no
        /// requieren GC variant porque no anaden ni quitan refs.
        const char     *native_fn_gc;
        PrimitiveKind   ret;              ///< Tipo de retorno semantico.
        int             n_args;           ///< Numero de args (sin contar this).
        PrimitiveKind   arg_types[3];     ///< Tipos de los args (max 3).
    };

    /**
     * @brief Tabla de tipos coleccion + sus constructores / liberadores.
     */
    static constexpr ColType COL_TYPES[] = {
        { PrimitiveKind::ARRAYLIST, "arraylist", "vcol_alist_new", "vcol_alist_free", "vcol_alist_free_gc", 16 },
        { PrimitiveKind::HASHMAP,   "hashmap",   "vcol_map_new",   "vcol_map_free",   "vcol_map_free_gc",   16 },
        { PrimitiveKind::HASHSET,   "hashset",   "vcol_set_new",   "vcol_set_free",   "vcol_set_free_gc",   16 },
        { PrimitiveKind::QUEUE,     "queue",     "vcol_queue_new", "vcol_queue_free", "vcol_queue_free_gc", 16 },
        { PrimitiveKind::DEQUE,     "deque",     "vcol_deque_new", "vcol_deque_free", "vcol_deque_free_gc", 16 },
        { PrimitiveKind::TREEMAP,   "treemap",   "vcol_tmap_new",  "vcol_tmap_free",  "vcol_tmap_free_gc",  0  },
        { PrimitiveKind::TREESET,   "treeset",   "vcol_tset_new",  "vcol_tset_free",  "vcol_tset_free_gc",  0  },
        { PrimitiveKind::STACK,     "stack",     "vcol_alist_new", "vcol_alist_free", "vcol_alist_free_gc", 16 }, // Stack = alist con LIFO
    };
    static constexpr size_t COL_TYPES_N = sizeof(COL_TYPES) / sizeof(COL_TYPES[0]);

    /**
     * @brief Tabla de metodos de coleccion + sus extern fns nativos.
     */
    static constexpr ColMethod COL_METHODS[] = {
        // ===== ArrayList =====
        // Cada fila: { type, vex_name, native_fn, native_fn_gc, ret, n_args, arg_types }
        // native_fn_gc = nullptr si la op es read-only (no anade ni quita refs).
        { PrimitiveKind::ARRAYLIST, "push",     "vcol_alist_push",     "vcol_alist_push_gc",     PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "pop",      "vcol_alist_pop",      "vcol_alist_pop_gc",      PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::ARRAYLIST, "get",      "vcol_alist_get",      nullptr,                  PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "set",      "vcol_alist_set",      "vcol_alist_set_gc",      PrimitiveKind::I64, 2, { PrimitiveKind::I64, PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "size",     "vcol_alist_size",     nullptr,                  PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::ARRAYLIST, "clear",    "vcol_alist_clear",    "vcol_alist_clear_gc",    PrimitiveKind::VOID,0, {} },
        { PrimitiveKind::ARRAYLIST, "remove_at","vcol_alist_remove_at","vcol_alist_remove_at_gc",PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "insert",   "vcol_alist_insert",   "vcol_alist_insert_gc",   PrimitiveKind::I64, 2, { PrimitiveKind::I64, PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "indexof",  "vcol_alist_indexof",  nullptr,                  PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::ARRAYLIST, "sort",     "vcol_alist_sort",     nullptr,                  PrimitiveKind::VOID,0, {} },

        // ===== HashMap =====
        { PrimitiveKind::HASHMAP, "put",      "vcol_map_put",      "vcol_map_put_gc",    PrimitiveKind::VOID, 2, { PrimitiveKind::I64, PrimitiveKind::I64 } },
        { PrimitiveKind::HASHMAP, "get",      "vcol_map_get",      nullptr,              PrimitiveKind::I64,  1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHMAP, "contains", "vcol_map_contains", nullptr,              PrimitiveKind::I64,  1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHMAP, "remove",   "vcol_map_remove",   "vcol_map_remove_gc", PrimitiveKind::I64,  1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHMAP, "size",     "vcol_map_size",     nullptr,              PrimitiveKind::I64,  0, {} },
        { PrimitiveKind::HASHMAP, "clear",    "vcol_map_clear",    "vcol_map_clear_gc",  PrimitiveKind::VOID, 0, {} },
        { PrimitiveKind::HASHMAP, "keys",     "vcol_map_keys",     nullptr,              PrimitiveKind::ARRAYLIST, 0, {} },
        { PrimitiveKind::HASHMAP, "values",   "vcol_map_values",   nullptr,              PrimitiveKind::ARRAYLIST, 0, {} },

        // ===== HashSet =====
        { PrimitiveKind::HASHSET, "add",      "vcol_set_add",      "vcol_set_add_gc",    PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHSET, "contains", "vcol_set_contains", nullptr,              PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHSET, "remove",   "vcol_set_remove",   "vcol_set_remove_gc", PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::HASHSET, "size",     "vcol_set_size",     nullptr,              PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::HASHSET, "clear",    "vcol_set_clear",    "vcol_set_clear_gc",  PrimitiveKind::VOID,0, {} },
        { PrimitiveKind::HASHSET, "to_list",  "vcol_set_to_list",  nullptr,              PrimitiveKind::ARRAYLIST, 0, {} },

        // ===== Queue =====
        { PrimitiveKind::QUEUE, "push", "vcol_queue_push", "vcol_queue_push_gc",  PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::QUEUE, "pop",  "vcol_queue_pop",  "vcol_queue_pop_gc",   PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::QUEUE, "peek", "vcol_queue_peek", nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::QUEUE, "size", "vcol_queue_size", nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::QUEUE, "clear","vcol_queue_clear","vcol_queue_clear_gc", PrimitiveKind::VOID,0, {} },

        // ===== Deque =====
        { PrimitiveKind::DEQUE, "push_back",  "vcol_deque_push_back",  "vcol_deque_push_back_gc",  PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::DEQUE, "push_front", "vcol_deque_push_front", "vcol_deque_push_front_gc", PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::DEQUE, "pop_back",   "vcol_deque_pop_back",   "vcol_deque_pop_back_gc",   PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::DEQUE, "pop_front",  "vcol_deque_pop_front",  "vcol_deque_pop_front_gc",  PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::DEQUE, "peek_back",  "vcol_deque_peek_back",  nullptr,                    PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::DEQUE, "peek_front", "vcol_deque_peek_front", nullptr,                    PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::DEQUE, "size",       "vcol_deque_size",       nullptr,                    PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::DEQUE, "clear",      "vcol_deque_clear",      "vcol_deque_clear_gc",      PrimitiveKind::VOID,0, {} },

        // ===== TreeMap =====
        { PrimitiveKind::TREEMAP, "put",         "vcol_tmap_put",         "vcol_tmap_put_gc",    PrimitiveKind::VOID,2, { PrimitiveKind::I64, PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "get",         "vcol_tmap_get",         nullptr,               PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "contains",    "vcol_tmap_contains",    nullptr,               PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "remove",      "vcol_tmap_remove",      "vcol_tmap_remove_gc", PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "size",        "vcol_tmap_size",        nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREEMAP, "clear",       "vcol_tmap_clear",       "vcol_tmap_clear_gc",  PrimitiveKind::VOID,0, {} },
        { PrimitiveKind::TREEMAP, "first_key",   "vcol_tmap_first_key",   nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREEMAP, "last_key",    "vcol_tmap_last_key",    nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREEMAP, "floor_key",   "vcol_tmap_floor_key",   nullptr,               PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "ceiling_key", "vcol_tmap_ceiling_key", nullptr,               PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREEMAP, "keys",        "vcol_tmap_keys",        nullptr,               PrimitiveKind::ARRAYLIST, 0, {} },
        { PrimitiveKind::TREEMAP, "values",      "vcol_tmap_values",      nullptr,               PrimitiveKind::ARRAYLIST, 0, {} },

        // ===== TreeSet =====
        { PrimitiveKind::TREESET, "add",      "vcol_tset_add",      "vcol_tset_add_gc",    PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREESET, "contains", "vcol_tset_contains", nullptr,               PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREESET, "remove",   "vcol_tset_remove",   "vcol_tset_remove_gc", PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::TREESET, "size",     "vcol_tset_size",     nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREESET, "clear",    "vcol_tset_clear",    "vcol_tset_clear_gc",  PrimitiveKind::VOID,0, {} },
        { PrimitiveKind::TREESET, "first",    "vcol_tset_first",    nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREESET, "last",     "vcol_tset_last",     nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::TREESET, "to_list",  "vcol_tset_to_list",  nullptr,               PrimitiveKind::ARRAYLIST, 0, {} },

        // ===== Stack (LIFO sobre alist) =====
        { PrimitiveKind::STACK, "push", "vcol_alist_push", "vcol_alist_push_gc",  PrimitiveKind::I64, 1, { PrimitiveKind::I64 } },
        { PrimitiveKind::STACK, "pop",  "vcol_alist_pop",  "vcol_alist_pop_gc",   PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::STACK, "size", "vcol_alist_size", nullptr,               PrimitiveKind::I64, 0, {} },
        { PrimitiveKind::STACK, "clear","vcol_alist_clear","vcol_alist_clear_gc", PrimitiveKind::VOID,0, {} },
    };
    static constexpr size_t COL_METHODS_N = sizeof(COL_METHODS) / sizeof(COL_METHODS[0]);

    /**
     * @brief Devuelve el ColType correspondiente a @p kind, o nullptr si no
     *        es un tipo primitivo de coleccion.
     */
    inline const ColType *find_col_type(PrimitiveKind kind) {
        for (size_t i = 0; i < COL_TYPES_N; ++i) {
            if (COL_TYPES[i].kind == kind) return &COL_TYPES[i];
        }
        return nullptr;
    }

    /**
     * @brief Devuelve el ColType cuyo @c vex_ctor_name coincide con @p name,
     *        o nullptr si @p name no es un constructor de coleccion.
     */
    inline const ColType *find_col_ctor(const std::string &name) {
        for (size_t i = 0; i < COL_TYPES_N; ++i) {
            if (name == COL_TYPES[i].vex_ctor_name) return &COL_TYPES[i];
        }
        return nullptr;
    }

    /**
     * @brief Busca un metodo @p method_name aplicable al tipo receiver @p kind.
     *        Devuelve nullptr si no existe ninguna entrada que coincida.
     */
    inline const ColMethod *find_col_method(PrimitiveKind kind, const std::string &method_name) {
        for (size_t i = 0; i < COL_METHODS_N; ++i) {
            if (COL_METHODS[i].type == kind && method_name == COL_METHODS[i].vex_name) {
                return &COL_METHODS[i];
            }
        }
        return nullptr;
    }

    /** @brief @c true si @p k es uno de los PrimitiveKind de coleccion. */
    inline bool is_col_kind(PrimitiveKind k) {
        return k == PrimitiveKind::ARRAYLIST
            || k == PrimitiveKind::HASHMAP
            || k == PrimitiveKind::HASHSET
            || k == PrimitiveKind::QUEUE
            || k == PrimitiveKind::DEQUE
            || k == PrimitiveKind::TREEMAP
            || k == PrimitiveKind::TREESET
            || k == PrimitiveKind::STACK;
    }

    /**
     * @brief @c true si @p k designa un tipo cuyo handle vive en el GC heap
     *        de VestaVM y por tanto requiere write-barrier al guardarse en
     *        una coleccion nativa.  Cubre @c string (StringObject),
     *        instancias de clase y futuros / closures (que tambien viven
     *        como objetos GC).
     *
     * Cero coste cuando la respuesta es @c false: el lowering despacha a la
     * variante no-GC del builtin sin emitir @c getproc adicional.
     */
    inline bool is_gc_kind(PrimitiveKind k) {
        return k == PrimitiveKind::STRING
            || k == PrimitiveKind::CLASS
            || k == PrimitiveKind::FUNCTION;
    }

    /**
     * @brief Decide si una coleccion declarada con un determinado tipo de
     *        elemento (@p element_kind) o, en mapas, valor (@p value_kind)
     *        debe usar las variantes @c *_gc.  Para @c HashMap / @c TreeMap
     *        @p value_kind es el tipo del @c value (las keys son uint64
     *        opacas por convencion, no participan en el barrier).  Para los
     *        no-map se ignora @p value_kind.
     */
    inline bool col_needs_gc_aware(PrimitiveKind container, PrimitiveKind element_kind,
                                    PrimitiveKind value_kind = PrimitiveKind::VOID)
    {
        if (container == PrimitiveKind::HASHMAP || container == PrimitiveKind::TREEMAP) {
            // En el modelo actual del plugin solo el VALUE puede ser GC (las
            // keys son uint64 puras).  Si el value es GC -> gc-aware.
            return is_gc_kind(value_kind);
        }
        return is_gc_kind(element_kind);
    }

} // namespace vex
