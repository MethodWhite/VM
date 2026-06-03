/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef STRING_INTERN_H
#define STRING_INTERN_H

/**
 * @file string_intern.h
 * @brief Pool de interning de cadenas por proceso (StringInternPool).
 *
 * El pool mapea el contenido en bruto de un string FLAT (bytes + encoding)
 * a su GcHandle canonico.  Cuando un string se interna:
 *   1. Se calcula su clave = raw bytes + byte de encoding.
 *   2. Si ya existe una entrada con la misma clave, se devuelve ese GcHandle.
 *   3. Si no existe, se anade la entrada y se marca el StringObject con
 *      STR_INTERNED_FLAG para indicar que pertenece al pool.
 *
 * El pool vive en memoria host (no en VM mem) y se destruye al finalizar
 * el proceso VM.  Esta vinculado al ProcessVM mediante un puntero opcional.
 *
 * Limitaciones de la implementacion actual:
 *   - Solo se internan strings FLAT (ROPE/SLICE deben materializarse primero).
 *   - No hay expulsion automatica; los strings internados viven hasta que el
 *     proceso termina.
 *   - El pool no esta sincronizado; cada ProcessVM tiene el suyo.
 */

#include "gc/gc_heap.h"
#include <unordered_map>
#include <string>

namespace runtime {

    /**
     * @brief Pool de interning de strings por proceso.
     *
     * La clave del mapa es la concatenacion de:
     *   bytes del string (byte_len bytes) + un byte con el valor de encoding
     * lo que garantiza que dos strings con el mismo contenido pero diferente
     * codificacion no se confunden.
     */
    class StringInternPool {
    public:
        /**
         * @brief Busca o inserta un string en el pool.
         *
         * @param key     Clave: raw bytes + encoding byte.
         * @param handle  GcHandle del StringObject a internar.
         * @return        GcHandle del string canonico del pool.
         *                Puede ser @p handle (insercion) u otro (string ya existente).
         */
        gc::GcHandle intern(const std::string &key, gc::GcHandle handle) {
            auto it = map_.find(key);
            if (it != map_.end()) return it->second; // string ya internado
            map_.emplace(key, handle);                // nuevo entry en el pool
            return handle;
        }

        /**
         * @brief Lookup rapido por hash, sin std::string heap alloc.
         *
         * Sprint string-perf (2026-06-02): la version original llamaba
         * @c make_intern_key que aloca un std::string nuevo CADA STRMAKE
         * (copia bytes + concatena encoding byte).  Ese heap alloc + el
         * hash de string completo costaban ~150-200 ns por intern call.
         *
         * Esta variante computa FNV-1a sobre (bytes + enc) inline y mantiene
         * un mapa @c hash -> @c GcHandle paralelo al principal.  Los hash
         * collisions se validan via byte-compare contra el StringObject
         * apuntado.  Tras un hit, el caller skipea TODO el flujo de
         * @c alloc_flat (ahorra ~200 ns adicionales).
         *
         * Caller debe deref el handle devuelto + verificar que su byte_len
         * y bytes matchean los que esperaba (defensa anti-colision).
         */
        gc::GcHandle lookup_by_hash(uint64_t h) const {
            auto it = hash_map_.find(h);
            if (it != hash_map_.end()) return it->second;
            return gc::GC_NULL_HANDLE;
        }

        /** @brief Inserta una entrada hash -> handle.  Llamar tras alloc. */
        void insert_by_hash(uint64_t h, gc::GcHandle handle) {
            hash_map_.emplace(h, handle);
        }

        /**
         * @brief Comprueba si existe una entrada con la clave dada.
         *
         * @param key  Clave: raw bytes + encoding byte.
         * @return     GcHandle canonico o GC_NULL_HANDLE si no existe.
         */
        gc::GcHandle lookup(const std::string &key) const {
            auto it = map_.find(key);
            if (it != map_.end()) return it->second;
            return gc::GC_NULL_HANDLE;
        }

        /** @brief Numero de strings actualmente en el pool. */
        size_t size() const { return map_.size(); }

        /** @brief Vacia el pool (los objetos en el heap no se liberan aqui). */
        void clear() { map_.clear(); }

    private:
        std::unordered_map<std::string, gc::GcHandle> map_; ///< clave -> GcHandle canonico
        std::unordered_map<uint64_t, gc::GcHandle> hash_map_; ///< hash FNV-1a -> handle (fast path)
    };

} // namespace runtime

#endif // STRING_INTERN_H
