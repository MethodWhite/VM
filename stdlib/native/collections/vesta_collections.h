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
 * @file vesta_collections.h
 * @brief API publica de las colecciones nativas de VestaVM.
 *
 * Este header define las funciones exportadas por el plugin vesta_collections.
 * Todas las funciones siguen la convencion de llamada de VestaVM:
 *   - Argumentos en r1..r12 como uint64_t.
 *   - Valor de retorno en r0 como uint64_t.
 *   - Sin argumento de contexto implicito.
 *
 * Las colecciones disponibles son:
 *
 *   VestaList      -- array dinamico de GcHandle (uint32_t, 4 bytes por elemento).
 *                     Optimizado para referencias a objetos GC de VestaVM.
 *
 *   VestaArrayList -- array dinamico de uint64_t (8 bytes por elemento).
 *                     Para valores primitivos o mezcla de GcHandle y raw values.
 *
 *   VestaHashMap   -- tabla hash de clave-valor uint64_t -> uint64_t.
 *                     Direccionamiento abierto con sondeo lineal, carga maxima 0.75.
 *
 *   VestaHashSet   -- tabla hash de elementos uint64_t unicos.
 *                     Internamente un VestaHashMap con valor sentinel.
 *
 * Todos los handles son punteros host casteados a uint64_t.  El VM programmer
 * almacena el handle en un registro VM y lo pasa a las funciones de la coleccion.
 *
 * NOTA DE GC: los GcHandle almacenados en colecciones no son raices del GC.
 * Si el unico origen de un GcHandle es una coleccion nativa, el GC puede
 * recolectar el objeto referenciado.  Para evitarlo, el bytecode debe mantener
 * una copia del GcHandle en un registro VM o en memoria VM.
 *
 * Importar desde .vel:
 * @code
 *   @Import {
 *       @Method { @Lib("vesta_collections") @Name("vcol_list_new")      }
 *       @Method { @Lib("vesta_collections") @Name("vcol_list_push")     }
 *       @Method { @Lib("vesta_collections") @Name("vcol_list_get")      }
 *       @Method { @Lib("vesta_collections") @Name("vcol_list_size")     }
 *       @Method { @Lib("vesta_collections") @Name("vcol_list_free")     }
 *       @Method { @Lib("vesta_collections") @Name("vcol_alist_new")     }
 *       @Method { @Lib("vesta_collections") @Name("vcol_alist_push")    }
 *       @Method { @Lib("vesta_collections") @Name("vcol_alist_get")     }
 *       @Method { @Lib("vesta_collections") @Name("vcol_alist_size")    }
 *       @Method { @Lib("vesta_collections") @Name("vcol_alist_free")    }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_new")       }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_put")       }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_get")       }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_contains")  }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_remove")    }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_size")      }
 *       @Method { @Lib("vesta_collections") @Name("vcol_map_free")      }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_new")       }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_add")       }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_contains")  }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_remove")    }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_size")      }
 *       @Method { @Lib("vesta_collections") @Name("vcol_set_free")      }
 *   }
 * @endcode
 */

#ifndef VESTA_COLLECTIONS_H
#define VESTA_COLLECTIONS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Macro de exportacion (coherente con VestaPlugin.cmake)
 * ========================================================================= */

#ifndef VESTA_PLUGIN_EXPORT
#  if defined(_WIN32) || defined(__MINGW32__)
#    define VESTA_PLUGIN_EXPORT __declspec(dllexport)
#  elif defined(__GNUC__) || defined(__clang__)
#    define VESTA_PLUGIN_EXPORT __attribute__((visibility("default")))
#  else
#    define VESTA_PLUGIN_EXPORT
#  endif
#endif

/* =========================================================================
 * Valor centinela para "no encontrado" o "elemento vacio"
 * ========================================================================= */

/** @brief Valor de retorno cuando un elemento no se encuentra en la coleccion. */
#define VCOL_NOT_FOUND  UINT64_MAX

/** @brief Handle nulo (coleccion invalida o error de asignacion). */
#define VCOL_NULL       ((uint64_t)0)

/* =========================================================================
 * VestaList -- array dinamico de GcHandle (uint32_t por elemento)
 *
 * Almacena enteros de 32 bits (GcHandles del heap GC de VestaVM).
 * Capacidad inicial minima: 8 elementos.  Factor de crecimiento: x2.
 * ========================================================================= */

/**
 * @brief Crea una nueva VestaList con capacidad inicial dada.
 * @param initial_capacity Capacidad inicial en numero de elementos (minimo 8).
 * @return Handle (uint64_t puntero a VestaList*), o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_new(uint64_t initial_capacity);

/**
 * @brief Libera la memoria de una VestaList.
 * @param handle Handle devuelto por vcol_list_new.
 */
VESTA_PLUGIN_EXPORT void vcol_list_free(uint64_t handle);

/**
 * @brief Anade un elemento al final de la lista.  Redimensiona si es necesario.
 * @param handle  Handle de la lista.
 * @param element GcHandle (32 bits, en los 4 bytes bajos del uint64_t).
 * @return Nuevo tamano de la lista, o VCOL_NOT_FOUND si handle es nulo.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_push(uint64_t handle, uint64_t element);

/**
 * @brief Extrae y devuelve el ultimo elemento de la lista.
 * @param handle Handle de la lista.
 * @return Elemento extraido, o VCOL_NOT_FOUND si la lista esta vacia.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_pop(uint64_t handle);

/**
 * @brief Devuelve el elemento en la posicion indicada sin eliminarlo.
 * @param handle Handle de la lista.
 * @param index  Indice (base 0).
 * @return Elemento, o VCOL_NOT_FOUND si index >= size.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_get(uint64_t handle, uint64_t index);

/**
 * @brief Escribe un elemento en la posicion indicada.
 * @param handle  Handle de la lista.
 * @param index   Indice (base 0).
 * @param element Nuevo valor.
 * @return Valor anterior, o VCOL_NOT_FOUND si index >= size.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_set(uint64_t handle, uint64_t index,
                                            uint64_t element);

/** @brief Devuelve el numero de elementos en la lista. */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_size(uint64_t handle);

/** @brief Devuelve la capacidad actual de la lista (slots asignados). */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_cap(uint64_t handle);

/** @brief Elimina todos los elementos sin liberar la capacidad. */
VESTA_PLUGIN_EXPORT void vcol_list_clear(uint64_t handle);

/**
 * @brief Elimina el elemento en la posicion indicada desplazando el resto.
 * @param handle Handle de la lista.
 * @param index  Indice a eliminar.
 * @return Elemento eliminado, o VCOL_NOT_FOUND si index >= size.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_remove_at(uint64_t handle, uint64_t index);

/**
 * @brief Inserta un elemento en la posicion indicada desplazando el resto.
 * @param handle  Handle de la lista.
 * @param index   Posicion de insercion (0 = antes del primero).
 * @param element Elemento a insertar.
 * @return Nuevo tamano, o VCOL_NOT_FOUND si falla la reasignacion.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_insert(uint64_t handle, uint64_t index,
                                               uint64_t element);

/**
 * @brief Busca la primera ocurrencia de un elemento.
 * @param handle  Handle de la lista.
 * @param element Valor a buscar (comparacion exacta de 32 bits bajos).
 * @return Indice de la primera ocurrencia, o VCOL_NOT_FOUND si no esta.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_indexof(uint64_t handle, uint64_t element);

/**
 * @brief Clona la lista (copia superficial de los handles).
 * @param handle Handle de la lista original.
 * @return Handle de la nueva lista clonada, o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_list_clone(uint64_t handle);

/* =========================================================================
 * VestaArrayList -- array dinamico de uint64_t (8 bytes por elemento)
 *
 * Para valores primitivos o mezcla de valores raw y GcHandles de 64 bits.
 * Misma semantica de push/pop/get que VestaList pero con slots de 64 bits.
 * ========================================================================= */

/** @brief Crea un nuevo VestaArrayList con capacidad inicial dada. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_new(uint64_t initial_capacity);

/** @brief Libera la memoria de un VestaArrayList. */
VESTA_PLUGIN_EXPORT void vcol_alist_free(uint64_t handle);

/** @brief Anade un valor uint64_t al final.  Redimensiona si es necesario. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_push(uint64_t handle, uint64_t value);

/** @brief Extrae y devuelve el ultimo elemento. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_pop(uint64_t handle);

/** @brief Devuelve el elemento en la posicion indicada. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_get(uint64_t handle, uint64_t index);

/** @brief Escribe un elemento en la posicion indicada. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_set(uint64_t handle, uint64_t index,
                                             uint64_t value);

/** @brief Devuelve el numero de elementos. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_size(uint64_t handle);

/** @brief Devuelve la capacidad actual en slots. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_cap(uint64_t handle);

/** @brief Elimina todos los elementos sin liberar la capacidad. */
VESTA_PLUGIN_EXPORT void vcol_alist_clear(uint64_t handle);

/** @brief Elimina el elemento en la posicion indicada. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_remove_at(uint64_t handle, uint64_t index);

/** @brief Inserta un elemento en la posicion indicada. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_insert(uint64_t handle, uint64_t index,
                                                uint64_t value);

/** @brief Busca la primera ocurrencia de un valor (comparacion exacta de 64 bits). */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_indexof(uint64_t handle, uint64_t value);

/** @brief Clona el ArrayList. */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_clone(uint64_t handle);

/**
 * @brief Ordena el ArrayList con qsort ascendente (comparacion uint64_t).
 * @param handle Handle del ArrayList a ordenar.
 */
VESTA_PLUGIN_EXPORT void vcol_alist_sort(uint64_t handle);

/* =========================================================================
 * VestaHashMap -- tabla hash uint64_t -> uint64_t
 *
 * Direccionamiento abierto, sondeo lineal, factor de carga maximo 0.75.
 * Hash de clave: FNV-1a de 64 bits aplicado sobre los 8 bytes de la clave.
 * Capacidad siempre potencia de 2 para usar mascara en lugar de modulo.
 *
 * VCOL_NOT_FOUND (UINT64_MAX) es el sentinel de slot vacio en la tabla.
 * Las claves con valor UINT64_MAX-1 son validas pero pueden colisionar con
 * el centinela de borrado; usar vcol_map_get_or para distinguir "no existe"
 * de "valor 0".
 * ========================================================================= */

/**
 * @brief Crea un nuevo VestaHashMap con capacidad inicial dada.
 * @param initial_capacity Capacidad inicial en slots (redondeada a potencia de 2, minimo 16).
 * @return Handle (uint64_t), o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_new(uint64_t initial_capacity);

/** @brief Libera la memoria del mapa. */
VESTA_PLUGIN_EXPORT void vcol_map_free(uint64_t handle);

/**
 * @brief Inserta o actualiza el par (key, value) en el mapa.
 * @param handle Handle del mapa.
 * @param key    Clave uint64_t.
 * @param value  Valor uint64_t.
 */
VESTA_PLUGIN_EXPORT void vcol_map_put(uint64_t handle, uint64_t key, uint64_t value);

/**
 * @brief Devuelve el valor asociado a la clave, o 0 si no existe.
 * @param handle Handle del mapa.
 * @param key    Clave a buscar.
 * @return Valor asociado, o 0 si la clave no existe.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_get(uint64_t handle, uint64_t key);

/**
 * @brief Devuelve el valor asociado o un valor por defecto si no existe.
 * @param handle       Handle del mapa.
 * @param key          Clave a buscar.
 * @param default_val  Valor devuelto si la clave no existe.
 * @return Valor almacenado, o default_val si la clave no existe.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_get_or(uint64_t handle, uint64_t key,
                                              uint64_t default_val);

/**
 * @brief Comprueba si la clave existe en el mapa.
 * @return 1 si existe, 0 si no existe.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_contains(uint64_t handle, uint64_t key);

/**
 * @brief Elimina la clave del mapa.
 * @return Valor que tenia la clave, o VCOL_NOT_FOUND si no existia.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_remove(uint64_t handle, uint64_t key);

/** @brief Devuelve el numero de pares clave-valor en el mapa. */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_size(uint64_t handle);

/** @brief Elimina todos los pares sin liberar la capacidad. */
VESTA_PLUGIN_EXPORT void vcol_map_clear(uint64_t handle);

/**
 * @brief Devuelve las claves del mapa como un VestaArrayList.
 * @param handle Handle del mapa.
 * @return Handle de un nuevo VestaArrayList con todas las claves.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_keys(uint64_t handle);

/**
 * @brief Devuelve los valores del mapa como un VestaArrayList.
 * @param handle Handle del mapa.
 * @return Handle de un nuevo VestaArrayList con todos los valores.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_map_values(uint64_t handle);

/* =========================================================================
 * VestaHashSet -- conjunto de uint64_t unicos
 *
 * Implementado internamente como VestaHashMap con valor sentinel UINT64_MAX-1.
 * ========================================================================= */

/**
 * @brief Crea un nuevo VestaHashSet con capacidad inicial dada.
 * @param initial_capacity Capacidad inicial (redondeada a potencia de 2, minimo 16).
 * @return Handle, o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_new(uint64_t initial_capacity);

/** @brief Libera el set. */
VESTA_PLUGIN_EXPORT void vcol_set_free(uint64_t handle);

/**
 * @brief Anade un elemento al set.
 * @return 1 si se inserto como nuevo, 0 si ya existia.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_add(uint64_t handle, uint64_t element);

/**
 * @brief Comprueba si el elemento esta en el set.
 * @return 1 si existe, 0 si no.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_contains(uint64_t handle, uint64_t element);

/**
 * @brief Elimina un elemento del set.
 * @return 1 si se elimino, 0 si no existia.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_remove(uint64_t handle, uint64_t element);

/** @brief Devuelve el numero de elementos en el set. */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_size(uint64_t handle);

/** @brief Elimina todos los elementos sin liberar la capacidad. */
VESTA_PLUGIN_EXPORT void vcol_set_clear(uint64_t handle);

/**
 * @brief Devuelve todos los elementos del set como un VestaArrayList.
 * @return Handle de un nuevo VestaArrayList.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_to_list(uint64_t handle);

/**
 * @brief Comprueba si set_a es subconjunto de set_b.
 * @return 1 si todos los elementos de set_a estan en set_b, 0 si no.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_is_subset(uint64_t set_a, uint64_t set_b);

/**
 * @brief Devuelve la union de dos sets como un nuevo VestaHashSet.
 * @return Handle del nuevo set, o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_union(uint64_t set_a, uint64_t set_b);

/**
 * @brief Devuelve la interseccion de dos sets como un nuevo VestaHashSet.
 * @return Handle del nuevo set, o VCOL_NULL si falla malloc.
 */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_intersect(uint64_t set_a, uint64_t set_b);

/* =========================================================================
 * VestaQueue / VestaDeque -- ring buffer FIFO / LIFO+FIFO de uint64_t
 *
 * Ring buffer de capacidad potencia de 2 con head y tail.  push/pop son
 * O(1) amortizado; resize x2 cuando se llena.  Usa mascara bit para wrap-
 * around (sin modulo).  Cero realloc de elementos en operaciones normales.
 *
 * Queue:  FIFO puro -- vcol_queue_push (tail) + vcol_queue_pop (head).
 * Deque:  ambos extremos -- push_front, push_back, pop_front, pop_back.
 * ========================================================================= */

VESTA_PLUGIN_EXPORT uint64_t vcol_queue_new(uint64_t initial_capacity);
VESTA_PLUGIN_EXPORT void     vcol_queue_free(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_push(uint64_t handle, uint64_t value);  /* push tail */
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_pop(uint64_t handle);                    /* pop head */
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_peek(uint64_t handle);                   /* peek head sin pop */
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_size(uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_queue_clear(uint64_t handle);

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_new(uint64_t initial_capacity);
VESTA_PLUGIN_EXPORT void     vcol_deque_free(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_back(uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_front(uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_back(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_front(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_peek_front(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_peek_back(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_size(uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_deque_clear(uint64_t handle);

/* =========================================================================
 * VestaTreeMap / VestaTreeSet -- arbol Red-Black ordenado uint64_t (A.25.2)
 *
 * Map ordenado por clave: provee acceso O(log n) por clave + iteracion en
 * orden + operaciones de rango (first / last / floor / ceiling).
 * Implementacion: arbol Red-Black con sentinela NIL compartido para
 * simplicidad de balanceo.  Comparacion de claves: uint64_t numerico
 * (caller debe usar comparable representation, e.g. punteros / hashes).
 *
 * Set: wrapper sobre Map con valor sentinel.
 * ========================================================================= */

VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_new(void);
VESTA_PLUGIN_EXPORT void     vcol_tmap_free(uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_tmap_put(uint64_t handle, uint64_t key, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_get(uint64_t handle, uint64_t key);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_contains(uint64_t handle, uint64_t key);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_remove(uint64_t handle, uint64_t key);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_size(uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_tmap_clear(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_first_key(uint64_t handle);  /* min key, o VCOL_NOT_FOUND */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_last_key(uint64_t handle);   /* max key */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_floor_key(uint64_t handle, uint64_t key);    /* mayor key <= key */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_ceiling_key(uint64_t handle, uint64_t key);  /* menor key >= key */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_keys(uint64_t handle);   /* alist con keys ordenadas */
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_values(uint64_t handle); /* alist con values en orden de keys */

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_new(void);
VESTA_PLUGIN_EXPORT void     vcol_tset_free(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_add(uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_contains(uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_remove(uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_size(uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_tset_clear(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_first(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_last(uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_to_list(uint64_t handle); /* alist en orden */

/* =========================================================================
 * Variantes GC-aware -- write-barrier para colecciones que retienen
 * GcHandles (e.g. ArrayList<string>).  Cada operacion que adquiere o libera
 * un slot llama a vesta_init->gc_addref/gc_release del proceso activo via
 * la API extendida v2.
 *
 * Convencion: las variantes gc-aware reciben @c proc_ptr como PRIMER
 * argumento adicional (handle al ProcessVM activo) para que el plugin
 * pueda invocar las callbacks gc_addref/gc_release del API.  El frontend
 * Vex emite @c getproc para obtener el proc_ptr antes del CALLN.
 *
 * Cero overhead vs variante no-gc cuando los elementos son i64 puros: el
 * frontend solo dispatcha a las gc_* cuando el tipo de elemento es GC
 * (string, class, future, closure).
 * ========================================================================= */

/* ArrayList */
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_push_gc(uint64_t proc, uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_pop_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_set_gc(uint64_t proc, uint64_t handle, uint64_t index, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_remove_at_gc(uint64_t proc, uint64_t handle, uint64_t index);
VESTA_PLUGIN_EXPORT uint64_t vcol_alist_insert_gc(uint64_t proc, uint64_t handle, uint64_t index, uint64_t value);
VESTA_PLUGIN_EXPORT void     vcol_alist_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_alist_free_gc(uint64_t proc, uint64_t handle);

/* HashMap (values son GC; las keys son uint64 puros por convencion) */
VESTA_PLUGIN_EXPORT void     vcol_map_put_gc(uint64_t proc, uint64_t handle, uint64_t key, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_map_remove_gc(uint64_t proc, uint64_t handle, uint64_t key);
VESTA_PLUGIN_EXPORT void     vcol_map_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_map_free_gc(uint64_t proc, uint64_t handle);

/* HashSet (elementos son GC) */
VESTA_PLUGIN_EXPORT uint64_t vcol_set_add_gc(uint64_t proc, uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT uint64_t vcol_set_remove_gc(uint64_t proc, uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT void     vcol_set_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_set_free_gc(uint64_t proc, uint64_t handle);

/* Queue / Deque */
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_push_gc(uint64_t proc, uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_queue_pop_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_queue_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_queue_free_gc(uint64_t proc, uint64_t handle);

VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_back_gc(uint64_t proc, uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_push_front_gc(uint64_t proc, uint64_t handle, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_back_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT uint64_t vcol_deque_pop_front_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_deque_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_deque_free_gc(uint64_t proc, uint64_t handle);

/* TreeMap / TreeSet */
VESTA_PLUGIN_EXPORT void     vcol_tmap_put_gc(uint64_t proc, uint64_t handle, uint64_t key, uint64_t value);
VESTA_PLUGIN_EXPORT uint64_t vcol_tmap_remove_gc(uint64_t proc, uint64_t handle, uint64_t key);
VESTA_PLUGIN_EXPORT void     vcol_tmap_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_tmap_free_gc(uint64_t proc, uint64_t handle);

VESTA_PLUGIN_EXPORT uint64_t vcol_tset_add_gc(uint64_t proc, uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT uint64_t vcol_tset_remove_gc(uint64_t proc, uint64_t handle, uint64_t element);
VESTA_PLUGIN_EXPORT void     vcol_tset_clear_gc(uint64_t proc, uint64_t handle);
VESTA_PLUGIN_EXPORT void     vcol_tset_free_gc(uint64_t proc, uint64_t handle);

/* =========================================================================
 * String ops nativas
 *
 * Operan sobre buffers crudos (host_ptr + byte_len) que el frontend Vex
 * obtiene via STRRAW / STRGETBYTES de un StringObject.  No alocan
 * StringObjects nuevos -- devuelven escalares (indices, longitudes, bools)
 * o modifican in-place; el frontend Vex se encarga de envolver / dividir
 * con STRMAKE segun necesite.
 *
 * Internamente usan memmem / memchr / memcmp (en glibc / msvcrt usan SIMD
 * cuando esta disponible; cero overhead de re-implementacion en assembly).
 * ========================================================================= */

/** Indice de la primera ocurrencia de needle en haystack, o UINT64_MAX si no. */
VESTA_PLUGIN_EXPORT uint64_t vstr_indexof(uint64_t haystack_ptr, uint64_t haystack_len,
                                           uint64_t needle_ptr,   uint64_t needle_len);
/** Indice de la primera ocurrencia desde @p start, o UINT64_MAX. */
VESTA_PLUGIN_EXPORT uint64_t vstr_indexof_from(uint64_t haystack_ptr, uint64_t haystack_len,
                                                 uint64_t needle_ptr,   uint64_t needle_len,
                                                 uint64_t start);
/** 1 si needle aparece en haystack, 0 si no. */
VESTA_PLUGIN_EXPORT uint64_t vstr_contains(uint64_t haystack_ptr, uint64_t haystack_len,
                                            uint64_t needle_ptr,   uint64_t needle_len);
/** 1 si haystack empieza por needle, 0 si no. */
VESTA_PLUGIN_EXPORT uint64_t vstr_starts_with(uint64_t haystack_ptr, uint64_t haystack_len,
                                                uint64_t needle_ptr,   uint64_t needle_len);
/** 1 si haystack termina con needle, 0 si no. */
VESTA_PLUGIN_EXPORT uint64_t vstr_ends_with(uint64_t haystack_ptr, uint64_t haystack_len,
                                              uint64_t needle_ptr,   uint64_t needle_len);
/** Convierte ASCII A-Z a a-z in-place.  No-op para bytes >= 0x80. */
VESTA_PLUGIN_EXPORT void     vstr_lower_inplace(uint64_t ptr, uint64_t len);
/** Convierte ASCII a-z a A-Z in-place.  No-op para bytes >= 0x80. */
VESTA_PLUGIN_EXPORT void     vstr_upper_inplace(uint64_t ptr, uint64_t len);
/** Devuelve cuantos bytes whitespace (' ', '\t', '\n', '\r') hay al inicio. */
VESTA_PLUGIN_EXPORT uint64_t vstr_trim_start_offset(uint64_t ptr, uint64_t len);
/** Devuelve longitud final tras quitar whitespace al final.  trim_total = len - start - end_strip. */
VESTA_PLUGIN_EXPORT uint64_t vstr_trim_end_strip(uint64_t ptr, uint64_t len);

/**
 * @brief Split de haystack por delim.  Devuelve un VestaArrayList handle
 *        con N elementos uint64_t empaquetados como (offset << 32) | len,
 *        donde offset es la posicion en haystack donde empieza la
 *        substring y len es su longitud en bytes (sin contar el delim).
 *
 * El caller en Vex itera el alist, descodifica cada elemento y hace
 * STRMAKE de la substring usando (haystack_ptr + offset, len) como
 * (raw_ptr, byte_len).  No incluye substrings vacios entre delims
 * consecutivos (skip).
 *
 * Coste: O(haystack_len) usando vstr_memmem (Boyer-Moore-Horspool para
 * delims largos, memchr para delim de 1 byte como '\n').
 *
 * @return Handle al alist resultado (caller debe vcol_alist_free al
 *         terminar), o VCOL_NULL si malloc falla.
 */
VESTA_PLUGIN_EXPORT uint64_t vstr_split_offsets(uint64_t haystack_ptr, uint64_t haystack_len,
                                                  uint64_t delim_ptr,    uint64_t delim_len);

/* =========================================================================
 * Array ops nativas 
 *
 * Operan sobre arrays crudos (host_ptr) de uint64_t.  El frontend Vex pasa
 * el host_ptr de un array nativo (T*) o de un alist (data ptr).
 * ========================================================================= */

/** Sort in-place de N uint64_t ascendente (qsort + comparador trivial). */
VESTA_PLUGIN_EXPORT void     varr_sort_u64(uint64_t ptr, uint64_t n);
/** Sort in-place de N uint64_t descendente. */
VESTA_PLUGIN_EXPORT void     varr_sort_u64_desc(uint64_t ptr, uint64_t n);
/** Busqueda binaria en array uint64_t ordenado asc.  Indice o UINT64_MAX. */
VESTA_PLUGIN_EXPORT uint64_t varr_bsearch_u64(uint64_t ptr, uint64_t n, uint64_t key);
/** Busqueda lineal en array uint64_t.  Indice o UINT64_MAX.  memchr-like sobre 64-bit. */
VESTA_PLUGIN_EXPORT uint64_t varr_indexof_u64(uint64_t ptr, uint64_t n, uint64_t key);
/** Reverse in-place de N uint64_t. */
VESTA_PLUGIN_EXPORT void     varr_reverse_u64(uint64_t ptr, uint64_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_COLLECTIONS_H */
