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
 * @file class_registry.h
 * @brief Registro global de clases definidas en runtime.
 *
 * @section design Modelo de POO dinamica
 *
 * VestaVM no almacena metadatos de clase como anotaciones estaticas en el
 * .velb.  En su lugar, las clases se definen invocando funciones del
 * @c ClassRegistry desde el bytecode (instrucciones @c defclass /
 * @c defmethod / @c deffield) o desde C++.  Esto unifica el modelo:
 *
 *   1. Las clases declaradas estaticamente en Vex se traducen a un
 *      bloque @c __module_init que invoca al registry al cargar el
 *      modulo.
 *   2. Programas de meta-programacion pueden crear clases nuevas en
 *      runtime sin pasar por el compilador.
 *   3. Otros modulos cargados con @c loadmodule registran sus clases
 *      en la misma tabla global.
 *
 * @section perf Rendimiento y costes
 *
 * El registry esta optimizado para proyectos grandes:
 *
 *   - Lookup de clase por nombre: O(1) amortizado (unordered_map).
 *   - Lookup de field/method por nombre dentro de una clase: O(1)
 *     amortizado (open addressing en @c ClassInfo::field_lookup_table /
 *     @c method_lookup_table, factor de carga 0.5).
 *   - Acceso por offset cacheado (NEWOBJ + GETFIELD por indice): 1
 *     instruccion VM (gcderef + addcur + readcur), sin lookup.
 *   - CALLVIRT por slot del vtable: 1 indireccion + 1 invocacion.
 *
 * @section aop Soporte de programacion de aspectos
 *
 * Cada @c MethodInfo lleva un puntero opcional @c advice_chain.  Si es
 * NULL, CALLVIRT toma el fast path (overhead cero).  Si no, CALLVIRT
 * recorre la lista en orden e invoca cada @c BEFORE / @c AFTER /
 * @c AROUND.  El registry expone @c add_advice() para anadirlos
 * dinamicamente.
 *
 * @section ownership Propiedad de memoria
 *
 * Toda la memoria de @c ClassInfo, @c FieldInfo[], @c MethodInfo[],
 * stringx::data, lookup tables, etc. la posee el registry y se libera
 * en su destructor.  Los @c ObjectHeader::class_ptr de objetos vivos
 * son punteros estables hasta que se descargue el modulo propietario.
 */

#ifndef CLASS_REGISTRY_H
#define CLASS_REGISTRY_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "loader/oop_types.h"

namespace loader {

    /**
     * @struct FieldDecl
     * @brief Descripcion de un campo a registrar via @c define_class.
     *
     * Permite construir @c FieldInfo[] de una clase sin manipular structs C
     * desde el caller.  El registry calcula offset y size finales.
     */
    struct FieldDecl {
        std::string  name;
        FieldKind    kind        = FIELD_PRIMITIVE;
        ClassInfo   *type_class  = nullptr; ///< usado solo si kind == FIELD_CLASS / FIELD_STRUCT
        uint32_t     size_bytes  = 8;       ///< tamano del slot (8 estandar; aceptamos 1/2/4/8)
        FieldAccess  access      = FIELD_PUBLIC;
        bool         is_static   = false;
    };

    /**
     * @struct MethodDecl
     * @brief Descripcion de un metodo a registrar via @c define_class.
     */
    struct MethodDecl {
        std::string  name;
        std::string  descriptor; ///< firma legible para reflexion
        uint64_t     flags       = 0;     ///< METHOD_FLAG_*
        uint64_t     code_vaddr  = 0;     ///< inicio del bytecode (0 si abstracto)
        size_t       code_size   = 0;
    };

    /**
     * @class ClassRegistry
     * @brief Tabla global de clases definidas en runtime.
     *
     * No es thread-safe; las modificaciones se serializan en el caller
     * (Loader o instrucciones VM ejecutadas en un proceso unico a la vez
     * para una clase concreta).  Las consultas concurrentes via
     * @c find_class son seguras siempre que no se este modificando.
     */
    class ClassRegistry {
    public:
        ClassRegistry()  = default;
        ~ClassRegistry() = default;

        ClassRegistry(const ClassRegistry &)            = delete;
        ClassRegistry &operator=(const ClassRegistry &) = delete;

        // -------------------------------------------------------------
        // API de definicion de clases
        // -------------------------------------------------------------

        /**
         * @brief Define una clase nueva y la registra en la tabla global.
         *
         * Calcula automaticamente:
         *   - Offsets de cada field (alineacion natural a 8 bytes; cada slot
         *     ocupa @c FieldDecl::size_bytes redondeado al multiplo de 8).
         *   - @c ClassInfo::instance_size = sizeof(ObjectHeader) + total fields.
         *   - Vtable: copia los metodos en orden de declaracion; si la clase
         *     hereda de un super, el vtable empieza con los metodos heredados.
         *   - Tablas hash de lookup para field y method por nombre.
         *
         * @param name        Nombre calificado de la clase.
         * @param super       ClassInfo de la superclase (NULL = Object por defecto).
         * @param interfaces  Punteros a interfaces implementadas (puede ser vacio).
         * @param fields      Campos de instancia y estaticos.
         * @param methods     Metodos (incluyendo constructores).
         * @param class_flags CLASS_FLAG_* bits.
         * @return Puntero estable al ClassInfo creado.  El registry mantiene
         *         la propiedad; no liberar manualmente.
         */
        ClassInfo *define_class(const std::string                &name,
                                ClassInfo                        *super,
                                const std::vector<ClassInfo *>   &interfaces,
                                const std::vector<FieldDecl>     &fields,
                                const std::vector<MethodDecl>    &methods,
                                uint64_t                          class_flags = CLASS_VIS_PUBLIC);

        /**
         * @brief Anade un nuevo campo a una clase ya definida.
         *
         * Reasigna @c fields[], recalcula offsets a partir del primer
         * campo nuevo, actualiza @c instance_size y reconstruye la tabla
         * hash de lookup.  Solo valido para clases que aun no tienen
         * instancias vivas (la VM no migra layouts).
         *
         * @return true si la operacion tuvo exito; false si ya existe un
         *         campo con ese nombre.
         */
        bool add_field(ClassInfo *cls, const FieldDecl &decl);

        /**
         * @brief Anade un nuevo metodo a una clase ya definida.
         *
         * Crece @c methods[] y, si el metodo es virtual, anade un slot al
         * @c vtable[].  Reconstruye la tabla hash.
         *
         * @return true si tuvo exito; false si ya existe un metodo con
         *         esa firma exacta (name + descriptor).
         */
        bool add_method(ClassInfo *cls, const MethodDecl &decl);

        /**
         * @brief Anade un advice (BEFORE / AFTER / AROUND) a un metodo.
         *
         * El advice se inserta al final de la cadena.  El advice_method
         * debe tener el mismo descriptor que el target (validado).
         *
         * @return true si se anadio; false si los descriptores no encajan.
         */
        bool add_advice(MethodInfo *target, uint8_t kind, MethodInfo *advice_method);

        // -------------------------------------------------------------
        // API de busqueda
        // -------------------------------------------------------------

        /**
         * @brief Localiza una clase por nombre cualificado.
         * @return ClassInfo* o nullptr si no existe.
         */
        ClassInfo *find_class(const std::string &name) const;

        /**
         * @brief Localiza un field por nombre dentro de una clase.
         *
         * Usa la tabla hash @c ClassInfo::field_lookup_table (O(1)
         * amortizado).  Si la tabla no esta inicializada (clase a medio
         * construir), recurre a busqueda lineal sobre @c fields[].
         *
         * @return FieldInfo* o nullptr si no existe.
         */
        static FieldInfo *find_field(ClassInfo *cls, const char *name, size_t name_len);
        static FieldInfo *find_field(ClassInfo *cls, const std::string &name) {
            return find_field(cls, name.data(), name.size());
        }

        /**
         * @brief Localiza un metodo por nombre.
         */
        static MethodInfo *find_method(ClassInfo *cls, const char *name, size_t name_len);
        static MethodInfo *find_method(ClassInfo *cls, const std::string &name) {
            return find_method(cls, name.data(), name.size());
        }

        // -------------------------------------------------------------
        // Dispatch de interfaz (itables)
        // -------------------------------------------------------------

        /**
         * @brief Devuelve (creando LAZY si hace falta) la @c ItableEntry de
         *        @p iface para la clase @p cls, dimensionada a @p count slots.
         *
         * Como las interfaces NO registran sus metodos en runtime, la entry se
         * crea con @p count slots a nullptr; cada slot se rellena por nombre en
         * @c resolve_itable_method la primera vez que se despacha ese indice.
         * Se cachea en @c cls->itables para siempre.
         *
         * Thread-safety: el fast path (entrada ya creada) es lock-free (lee un
         * unico puntero @c cls->itables, 8B alineado = atomico en x86/ARM64, y
         * recorre el array NULL-terminado hasta @c iface==0).  La creacion
         * serializa con @c itable_mutex_ y publica el array con barrera release
         * (los arrays viejos se retienen en los pools, asi un lector con el
         * puntero viejo sigue siendo correcto).
         *
         * @return Puntero estable a la @c ItableEntry, o nullptr si los
         *         argumentos son nulos.
         */
        ItableEntry *get_or_build_itable(ClassInfo *cls, ClassInfo *iface, uint32_t count);

        /**
         * @brief Resuelve el @c MethodInfo* concreto para el metodo de indice
         *        @p method_index de la interfaz @p iface sobre la clase @p cls.
         *
         * Crea/obtiene la itable (dimensionada a @p count) y, si el slot
         * @p method_index aun no esta resuelto, lo rellena por nombre
         * (@p method_name) y lo cachea (escritura idempotente, lock-free).  Tras
         * el primer dispatch de cada indice, las consultas son un indice puro.
         *
         * @return El @c MethodInfo* concreto, o nullptr si no se encuentra
         *         (programa mal-formado).
         */
        MethodInfo *resolve_itable_method(ClassInfo *cls, ClassInfo *iface,
                                          uint32_t count, uint32_t method_index,
                                          const char *method_name, size_t name_len);

        // -------------------------------------------------------------
        // Iteracion (para reflexion / debug)
        // -------------------------------------------------------------

        /// Numero total de clases registradas.
        size_t class_count() const { return all_classes_.size(); }

        /// Acceso indexado a las clases (orden de registro).  Para reflexion.
        ClassInfo *class_at(size_t i) const {
            return (i < all_classes_.size()) ? all_classes_[i].get() : nullptr;
        }

    private:
        /// Mapa nombre -> ClassInfo*.  Los punteros son propiedad de
        /// @c all_classes_.
        std::unordered_map<std::string, ClassInfo *> by_name_;

        /// Almacen de ClassInfo (gestiona ciclo de vida).
        std::vector<std::unique_ptr<ClassInfo>>      all_classes_;

        /// Almacen de buffers contiguos para fields/methods/strings; se
        /// guardan como vectores propios para que los punteros internos
        /// (FieldInfo*, MethodInfo*, char*) sean estables.
        std::vector<std::unique_ptr<FieldInfo[]>>    field_arrays_;
        std::vector<std::unique_ptr<MethodInfo[]>>   method_arrays_;
        std::vector<std::unique_ptr<MethodInfo *[]>> vtable_arrays_;
        std::vector<std::unique_ptr<ClassInfo *[]>>  iface_arrays_;
        std::vector<std::unique_ptr<char[]>>         string_pool_;
        std::vector<std::unique_ptr<LookupSlot[]>>   lookup_arrays_;
        std::vector<std::unique_ptr<AdviceEntry>>    advice_pool_;

        /// Pools de las itables construidas lazy (ver get_or_build_itable).
        /// Los arrays viejos NO se liberan al crecer (RCU-style: un lector
        /// lock-free puede tener el puntero viejo); se liberan todos en el
        /// destructor del registry.
        std::vector<std::unique_ptr<ItableEntry[]>>  itable_arrays_;
        std::vector<std::unique_ptr<MethodInfo *[]>> itable_method_arrays_;
        /// Serializa el build lazy de itables (el fast path es lock-free).
        std::mutex                                   itable_mutex_;

        // Helpers internos.
        stringx     intern_string(const std::string &s);
        LookupSlot *build_lookup_table(const std::vector<std::pair<std::string, uint32_t>> &entries,
                                       uint32_t &out_mask);
        void        rebuild_field_lookup(ClassInfo *cls);
        void        rebuild_method_lookup(ClassInfo *cls);
    };

    // -------------------------------------------------------------------------
    //  Hash y helpers de lookup (compartidos con el ejecutor)
    // -------------------------------------------------------------------------

    /**
     * @brief FNV-1a hash de 64 bits sobre una cadena de bytes.
     *
     * Devuelve siempre un valor != 0 para que el slot vacio (name_hash == 0)
     * sea distinguible.  Si el hash crudo cae a 0, retornamos 1 (colision
     * benigna, casi nunca ocurre).
     */
    inline uint64_t fnv1a_64(const char *data, size_t len) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i) {
            h ^= static_cast<uint8_t>(data[i]);
            h *= 1099511628211ULL;
        }
        return h ? h : 1ULL;
    }

    /**
     * @brief Busqueda en una tabla LookupSlot por nombre.  Open addressing
     *        con linear probing.  Devuelve UINT32_MAX si no encuentra.
     */
    inline uint32_t lookup_slot(const LookupSlot *table, uint32_t mask,
                                 const char *name, size_t len) noexcept {
        if (!table) return UINT32_MAX;
        const uint64_t h = fnv1a_64(name, len);
        uint32_t idx = static_cast<uint32_t>(h) & mask;
        for (uint32_t probes = 0; probes <= mask; ++probes) {
            const LookupSlot &s = table[idx];
            if (s.name_hash == 0) return UINT32_MAX;       // slot vacio: no hay match
            if (s.name_hash == h && s.name_len == len
             && std::memcmp(s.name_ptr, name, len) == 0) {
                return s.index;
            }
            idx = (idx + 1) & mask;
        }
        return UINT32_MAX;
    }

} // namespace loader

#endif // CLASS_REGISTRY_H
