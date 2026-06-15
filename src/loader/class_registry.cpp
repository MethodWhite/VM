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
 * @file class_registry.cpp
 * @brief Implementacion del registro global de clases con lookup hash y AOP.
 */

#include "loader/class_registry.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace loader {

    // ---------------------------------------------------------------------
    //  Helpers locales
    // ---------------------------------------------------------------------

    /**
     * @brief Redondea up al siguiente multiplo de @p align (potencia de 2).
     */
    static inline uint32_t align_up(uint32_t v, uint32_t align) noexcept {
        return (v + (align - 1)) & ~(align - 1);
    }

    /**
     * @brief Calcula la siguiente potencia de 2 >= @p v y >= 4.
     *
     * Tamanos minimos: 4 slots (capacidad util 2 con factor 0.5).
     */
    static inline uint32_t next_pow2(uint32_t v) noexcept {
        uint32_t p = 4;
        while (p < v) p <<= 1;
        return p;
    }

    // ---------------------------------------------------------------------
    //  Internado de strings
    //
    //  Cada @c stringx que llega a un ClassInfo / FieldInfo / MethodInfo
    //  debe sobrevivir mientras el registry exista.  Como std::string puede
    //  reubicar su buffer al moverse, copiamos los bytes a un buffer
    //  estable (unique_ptr<char[]>) propiedad del registry.
    // ---------------------------------------------------------------------

    stringx ClassRegistry::intern_string(const std::string &s) {
        const size_t n = s.size();
        auto buf = std::make_unique<char[]>(n + 1);
        if (n > 0) std::memcpy(buf.get(), s.data(), n);
        buf[n] = '\0'; // util si algo lo trata como C-string
        stringx out;
        out.data = reinterpret_cast<uint8_t *>(buf.get());
        out.size = static_cast<uint32_t>(n);
        string_pool_.push_back(std::move(buf));
        return out;
    }

    // ---------------------------------------------------------------------
    //  Construccion de tablas hash de lookup
    // ---------------------------------------------------------------------

    LookupSlot *ClassRegistry::build_lookup_table(
        const std::vector<std::pair<std::string, uint32_t>> &entries,
        uint32_t &out_mask)
    {
        if (entries.empty()) {
            out_mask = 0;
            return nullptr;
        }
        // Factor de carga 0.5: cap = 2 * n redondeado a potencia de 2.
        const uint32_t cap = next_pow2(static_cast<uint32_t>(entries.size()) * 2);
        out_mask = cap - 1;
        auto buf = std::make_unique<LookupSlot[]>(cap);
        for (uint32_t i = 0; i < cap; ++i) {
            buf[i].name_hash = 0; // marca slot vacio
            buf[i].name_len  = 0;
            buf[i].name_ptr  = nullptr;
            buf[i].index     = 0;
        }
        for (const auto &kv : entries) {
            const std::string &name = kv.first;
            const uint64_t h = fnv1a_64(name.data(), name.size());
            uint32_t idx = static_cast<uint32_t>(h) & out_mask;
            // Linear probing hasta encontrar slot vacio.
            while (buf[idx].name_hash != 0) {
                idx = (idx + 1) & out_mask;
            }
            buf[idx].name_hash = h;
            buf[idx].name_len  = static_cast<uint32_t>(name.size());
            // Apuntamos al string_pool_ ya internado.  Buscamos la entrada
            // mas reciente que coincida; el caller debe haber internado
            // el nombre antes de llamar a esta funcion.
            // Como optimizacion: el caller pasa un puntero a la stringx
            // ya internada, pero por simplicidad re-internamos aqui.
            // Para mantenerlo simple y correcto, internamos siempre.
            const stringx interned = intern_string(name);
            buf[idx].name_ptr = reinterpret_cast<const char *>(interned.data);
            buf[idx].index    = kv.second;
        }
        LookupSlot *raw = buf.get();
        lookup_arrays_.push_back(std::move(buf));
        return raw;
    }

    // ---------------------------------------------------------------------
    //  define_class
    // ---------------------------------------------------------------------

    ClassInfo *ClassRegistry::define_class(
        const std::string                &name,
        ClassInfo                        *super,
        const std::vector<ClassInfo *>   &interfaces,
        const std::vector<FieldDecl>     &fields,
        const std::vector<MethodDecl>    &methods,
        uint64_t                          class_flags)
    {
        // No permitimos redefinir clases ya registradas en este registry.
        if (by_name_.count(name)) return by_name_[name];

        auto cls = std::make_unique<ClassInfo>();
        std::memset(cls.get(), 0, sizeof(ClassInfo));
        cls->name = intern_string(name);
        cls->flags = class_flags;
        cls->visibility = MODULE_VIS_EXPORT;

        // ---- jerarquia ----
        if (super) {
            auto supers_buf = std::make_unique<ClassInfo *[]>(1);
            supers_buf[0] = super;
            cls->supers      = supers_buf.get();
            cls->super_count = 1;
            iface_arrays_.push_back(std::move(supers_buf));
        }
        if (!interfaces.empty()) {
            auto ifaces_buf = std::make_unique<ClassInfo *[]>(interfaces.size());
            for (size_t i = 0; i < interfaces.size(); ++i) ifaces_buf[i] = interfaces[i];
            cls->interfaces      = ifaces_buf.get();
            cls->interface_count = interfaces.size();
            iface_arrays_.push_back(std::move(ifaces_buf));
        }

        // ---- fields ----
        // Separamos en instancia y estaticos.  Para los de instancia
        // calculamos offset acumulado partiendo del final de
        // ObjectHeader (24 bytes).  Cada slot se redondea a 8 bytes
        // por simplicidad (cabe i8/i16/i32/i64/ptr/handle).
        std::vector<FieldDecl> inst_fields, stat_fields;
        for (const auto &fd : fields) {
            (fd.is_static ? stat_fields : inst_fields).push_back(fd);
        }

        // Si hay super, copiamos sus fields heredados PRIMERO al array
        // local.  Esto permite que add_field posterior continue offset y
        // que find_field/findhash localice campos heredados sin necesidad
        // de re-emitir deffield desde __module_init de la subclase.
        const size_t super_inst_count   = super ? super->field_count        : 0;
        const size_t super_stat_count   = super ? super->static_field_count : 0;
        const size_t super_method_count = super ? super->method_count       : 0;

        // Construir FieldInfo[] de instancia (heredados + propios).
        std::vector<std::pair<std::string, uint32_t>> field_entries; // name -> index para hash
        const size_t total_inst = super_inst_count + inst_fields.size();
        if (total_inst > 0) {
            auto buf = std::make_unique<FieldInfo[]>(total_inst);
            // Copia bit-a-bit de los heredados.  Mantenemos su offset
            // original (calculado en la jerarquia del super) para que
            // GETFIELD/SETFIELD funcione sin recompilar el bytecode del
            // super.
            uint32_t off = static_cast<uint32_t>(sizeof(ObjectHeader));
            if (super_inst_count > 0 && super->fields) {
                std::memcpy(buf.get(), super->fields,
                            sizeof(FieldInfo) * super_inst_count);
                const FieldInfo &last = buf[super_inst_count - 1];
                off = last.offset + align_up(last.size, 8u);
                for (size_t i = 0; i < super_inst_count; ++i) {
                    const FieldInfo &fi = buf[i];
                    std::string fname(reinterpret_cast<const char *>(fi.name.data),
                                       fi.name.size);
                    field_entries.emplace_back(std::move(fname),
                                                static_cast<uint32_t>(i));
                }
            }
            // Anadimos los fields propios de esta clase tras los heredados.
            for (size_t i = 0; i < inst_fields.size(); ++i) {
                const FieldDecl &fd = inst_fields[i];
                FieldInfo &fi = buf[super_inst_count + i];
                std::memset(&fi, 0, sizeof(FieldInfo));
                fi.name       = intern_string(fd.name);
                fi.access     = fd.access;
                fi.kind       = fd.kind;
                fi.type_class = fd.type_class;
                fi.size       = std::max<uint32_t>(fd.size_bytes, 1u);
                fi.offset     = off;
                fi.is_static  = false;
                off += align_up(fi.size, 8u);
                field_entries.emplace_back(fd.name,
                    static_cast<uint32_t>(super_inst_count + i));
            }
            cls->instance_size = off;
            cls->fields        = buf.get();
            cls->field_count   = total_inst;
            field_arrays_.push_back(std::move(buf));
        } else {
            cls->instance_size = static_cast<uint32_t>(sizeof(ObjectHeader));
        }

        // Construir FieldInfo[] de estaticos (heredados + propios).
        const size_t total_stat = super_stat_count + stat_fields.size();
        if (total_stat > 0) {
            auto buf = std::make_unique<FieldInfo[]>(total_stat);
            uint32_t off = 0;
            if (super_stat_count > 0 && super->static_fields) {
                std::memcpy(buf.get(), super->static_fields,
                            sizeof(FieldInfo) * super_stat_count);
                const FieldInfo &last = buf[super_stat_count - 1];
                off = last.offset + align_up(last.size, 8u);
                for (size_t i = 0; i < super_stat_count; ++i) {
                    const FieldInfo &fi = buf[i];
                    std::string fname(reinterpret_cast<const char *>(fi.name.data),
                                       fi.name.size);
                    field_entries.emplace_back(std::move(fname),
                        static_cast<uint32_t>(total_inst + i));
                }
            }
            for (size_t i = 0; i < stat_fields.size(); ++i) {
                const FieldDecl &fd = stat_fields[i];
                FieldInfo &fi = buf[super_stat_count + i];
                std::memset(&fi, 0, sizeof(FieldInfo));
                fi.name       = intern_string(fd.name);
                fi.access     = fd.access;
                fi.kind       = fd.kind;
                fi.type_class = fd.type_class;
                fi.size       = std::max<uint32_t>(fd.size_bytes, 1u);
                fi.offset     = off;
                fi.is_static  = true;
                off += align_up(fi.size, 8u);
                field_entries.emplace_back(fd.name,
                    static_cast<uint32_t>(total_inst + super_stat_count + i));
            }
            cls->static_fields      = buf.get();
            cls->static_field_count = total_stat;
            // Reservamos / copiamos el bloque de datos estaticos.  El
            // super pudo tener su propio bloque static_data; copiamos esa
            // porcion al inicio del nuevo bloque para que las clases
            // accedan a sus static heredados sin trampas adicionales.
            // NOTA: esto efectivamente DUPLICA el storage estatico:
            // la subclase tiene su propia copia de los static fields
            // del super.  En herencia clasica de Java los static son
            // compartidos; aqui la simetria con campos de instancia es
            // mas simple y no expone static heredados todavia.
            if (off > 0) {
                auto sd = std::make_unique<char[]>(off);
                std::memset(sd.get(), 0, off);
                if (super && super->static_data && super_stat_count > 0) {
                    const FieldInfo &slast = super->static_fields[super_stat_count - 1];
                    const uint32_t sbytes = slast.offset + align_up(slast.size, 8u);
                    std::memcpy(sd.get(), super->static_data, sbytes);
                }
                cls->static_data = reinterpret_cast<uint8_t *>(sd.get());
                string_pool_.push_back(std::move(sd));
            }
            field_arrays_.push_back(std::move(buf));
        }

        // ---- methods ----
        // Heredamos la vtable del super primero (cada slot apunta al
        // metodo del super).  Las llamadas posteriores a add_method
        // detectan duplicados por nombre y reemplazan el slot
        // (override), o anaden uno nuevo al final.
        std::vector<std::pair<std::string, uint32_t>> method_entries;
        const size_t total_methods = super_method_count + methods.size();
        if (total_methods > 0) {
            auto buf  = std::make_unique<MethodInfo[]>(total_methods);
            auto vtbl = std::make_unique<MethodInfo *[]>(total_methods);
            // Copiar metodos heredados.  Importante: actualizamos
            // owner_class al super que los definio (no a la subclase)
            // para que la reflexion devuelva la clase original.
            if (super_method_count > 0 && super->methods) {
                std::memcpy(buf.get(), super->methods,
                            sizeof(MethodInfo) * super_method_count);
                for (size_t i = 0; i < super_method_count; ++i) {
                    MethodInfo &mi = buf[i];
                    // Reseteamos advice_chain en la copia: AOP
                    // registrado en el super NO se hereda por defecto
                    // (decision de diseno @Aspect se aplica a
                    // la clase definida explicitamente en el patron).
                    mi.advice_chain = nullptr;
                    std::string mname(reinterpret_cast<const char *>(mi.name.data),
                                       mi.name.size);
                    method_entries.emplace_back(std::move(mname),
                        static_cast<uint32_t>(i));
                }
            }
            // Anadir metodos propios del prototipo al final.
            for (size_t i = 0; i < methods.size(); ++i) {
                const MethodDecl &md = methods[i];
                MethodInfo &mi = buf[super_method_count + i];
                std::memset(&mi, 0, sizeof(MethodInfo));
                mi.name        = intern_string(md.name);
                mi.descriptor  = intern_string(md.descriptor);
                mi.flags       = md.flags;
                mi.owner_class = cls.get();
                mi.code_vaddr  = md.code_vaddr;
                mi.code_size   = md.code_size;
                method_entries.emplace_back(md.name,
                    static_cast<uint32_t>(super_method_count + i));
            }
            for (size_t i = 0; i < total_methods; ++i) vtbl[i] = &buf[i];
            cls->methods      = buf.get();
            cls->method_count = total_methods;
            cls->vtable       = vtbl.get();
            cls->vtable_size  = total_methods;
            method_arrays_.push_back(std::move(buf));
            vtable_arrays_.push_back(std::move(vtbl));
        }

        // ---- tablas hash de lookup ----
        cls->field_lookup_table  = build_lookup_table(field_entries,  cls->field_lookup_mask);
        cls->method_lookup_table = build_lookup_table(method_entries, cls->method_lookup_mask);

        ClassInfo *raw = cls.get();
        by_name_[name] = raw;
        all_classes_.push_back(std::move(cls));
        return raw;
    }

    // ---------------------------------------------------------------------
    //  add_advice
    // ---------------------------------------------------------------------

    bool ClassRegistry::add_advice(MethodInfo *target, uint8_t kind,
                                    MethodInfo *advice_method) {
        if (!target || !advice_method) return false;
        // Validar firmas: deben coincidir descriptor a nivel de bytes.
        if (target->descriptor.size != advice_method->descriptor.size) return false;
        if (target->descriptor.size > 0
         && std::memcmp(target->descriptor.data, advice_method->descriptor.data,
                        target->descriptor.size) != 0) return false;

        auto entry = std::make_unique<AdviceEntry>();
        std::memset(entry.get(), 0, sizeof(AdviceEntry));
        entry->kind          = kind;
        entry->advice_method = advice_method;
        entry->next          = nullptr;
        // Insertar al final de la cadena para preservar orden de
        // insercion (BEFORE en orden, AFTER el ejecutor lo recorre al
        // reves dentro de un solo paso).
        if (target->advice_chain == nullptr) {
            target->advice_chain = entry.get();
        } else {
            AdviceEntry *cur = target->advice_chain;
            while (cur->next) cur = cur->next;
            cur->next = entry.get();
        }
        advice_pool_.push_back(std::move(entry));
        return true;
    }

    // ---------------------------------------------------------------------
    //  rebuild_field_lookup / rebuild_method_lookup
    //
    //  Tras cada add_field / add_method invalidamos la tabla hash y la
    //  reconstruimos con todos los miembros actuales.  Coste O(n) por
    //  llamada; aceptable porque la definicion de clase se hace una vez
    //  durante la inicializacion del modulo.
    // ---------------------------------------------------------------------

    void ClassRegistry::rebuild_field_lookup(ClassInfo *cls) {
        std::vector<std::pair<std::string, uint32_t>> entries;
        entries.reserve(cls->field_count + cls->static_field_count);
        for (size_t i = 0; i < cls->field_count; ++i) {
            const FieldInfo &fi = cls->fields[i];
            entries.emplace_back(
                std::string(reinterpret_cast<const char *>(fi.name.data), fi.name.size),
                static_cast<uint32_t>(i));
        }
        for (size_t i = 0; i < cls->static_field_count; ++i) {
            const FieldInfo &fi = cls->static_fields[i];
            entries.emplace_back(
                std::string(reinterpret_cast<const char *>(fi.name.data), fi.name.size),
                static_cast<uint32_t>(cls->field_count + i));
        }
        cls->field_lookup_table = build_lookup_table(entries, cls->field_lookup_mask);
    }

    void ClassRegistry::rebuild_method_lookup(ClassInfo *cls) {
        std::vector<std::pair<std::string, uint32_t>> entries;
        entries.reserve(cls->method_count);
        for (size_t i = 0; i < cls->method_count; ++i) {
            const MethodInfo &mi = cls->methods[i];
            entries.emplace_back(
                std::string(reinterpret_cast<const char *>(mi.name.data), mi.name.size),
                static_cast<uint32_t>(i));
        }
        cls->method_lookup_table = build_lookup_table(entries, cls->method_lookup_mask);
    }

    // ---------------------------------------------------------------------
    //  add_field
    //
    //  Anade un campo a una clase existente.  Realoca @c fields[] o
    //  @c static_fields[], recalcula el offset del nuevo campo a partir
    //  del ultimo y reconstruye el lookup hash.  La version anterior del
    //  array queda en field_arrays_ hasta el destructor (los offsets
    //  externos guardados sobre el array viejo se invalidan; esto es
    //  aceptable porque add_field solo deberia llamarse durante la
    //  inicializacion del modulo, antes de que existan instancias).
    // ---------------------------------------------------------------------

    bool ClassRegistry::add_field(ClassInfo *cls, const FieldDecl &decl) {
        if (!cls) return false;
        if (find_field(cls, decl.name) != nullptr) return false;

        const size_t old_count = decl.is_static
                                  ? cls->static_field_count
                                  : cls->field_count;
        FieldInfo *src_arr = decl.is_static
                              ? cls->static_fields
                              : cls->fields;
        auto new_arr = std::make_unique<FieldInfo[]>(old_count + 1);
        if (old_count > 0 && src_arr) {
            std::memcpy(new_arr.get(), src_arr, sizeof(FieldInfo) * old_count);
        }

        FieldInfo &fi = new_arr[old_count];
        std::memset(&fi, 0, sizeof(FieldInfo));
        fi.name       = intern_string(decl.name);
        fi.access     = decl.access;
        fi.kind       = decl.kind;
        fi.type_class = decl.type_class;
        fi.size       = std::max<uint32_t>(decl.size_bytes, 1u);
        fi.is_static  = decl.is_static;

        // Calcular offset a partir del campo anterior.
        uint32_t base = decl.is_static ? 0u : static_cast<uint32_t>(sizeof(ObjectHeader));
        if (old_count > 0) {
            const FieldInfo &prev = new_arr[old_count - 1];
            base = prev.offset + align_up(prev.size, 8u);
        }
        fi.offset = base;

        if (decl.is_static) {
            cls->static_fields      = new_arr.get();
            cls->static_field_count = old_count + 1;
            // Static_data necesita crecer.  Reservamos un nuevo bloque
            // del tamano apropiado y zero-init; los datos previos se
            // copian para preservar valores escritos por defmethod
            // anterior.  El caller no debe asumir punteros estables a
            // static_data antes del fin de la inicializacion.
            const uint32_t new_size = base + align_up(fi.size, 8u);
            auto sd = std::make_unique<char[]>(new_size);
            std::memset(sd.get(), 0, new_size);
            if (cls->static_data && base > 0) {
                std::memcpy(sd.get(), cls->static_data, base);
            }
            cls->static_data = reinterpret_cast<uint8_t *>(sd.get());
            string_pool_.push_back(std::move(sd));
        } else {
            cls->fields        = new_arr.get();
            cls->field_count   = old_count + 1;
            cls->instance_size = base + align_up(fi.size, 8u);
        }
        field_arrays_.push_back(std::move(new_arr));
        rebuild_field_lookup(cls);
        return true;
    }

    // ---------------------------------------------------------------------
    //  add_method
    //
    //  Realoca @c methods[] y @c vtable[] en cada llamada y reconstruye
    //  el lookup hash.  Igual que add_field, es una operacion de
    //  inicializacion: se asume que no hay frames activos del metodo
    //  antiguo cuando se anade uno nuevo.
    // ---------------------------------------------------------------------

    bool ClassRegistry::add_method(ClassInfo *cls, const MethodDecl &decl) {
        if (!cls) return false;

        // Detectar override: si ya existe un metodo con el mismo nombre,
        // sobrescribir el slot de la vtable manteniendo el vtable_index
        // (semantica Java/C++ para herencia simple).
        const uint32_t existing_idx = lookup_slot(cls->method_lookup_table,
                                                   cls->method_lookup_mask,
                                                   decl.name.data(),
                                                   decl.name.size());
        const bool is_override = (existing_idx != UINT32_MAX
                                && existing_idx < cls->method_count);

        const size_t old_count = cls->method_count;
        const size_t new_count = is_override ? old_count : (old_count + 1);
        auto new_arr = std::make_unique<MethodInfo[]>(new_count);
        if (old_count > 0 && cls->methods) {
            std::memcpy(new_arr.get(), cls->methods, sizeof(MethodInfo) * old_count);
        }

        const size_t target_idx = is_override ? existing_idx : old_count;
        MethodInfo &mi = new_arr[target_idx];
        std::memset(&mi, 0, sizeof(MethodInfo));
        mi.name        = intern_string(decl.name);
        mi.descriptor  = intern_string(decl.descriptor);
        mi.flags       = decl.flags;
        mi.owner_class = cls;
        mi.code_vaddr  = decl.code_vaddr;
        mi.code_size   = decl.code_size;

        // Reconstruir vtable manteniendo los slots heredados que no han
        // cambiado.  Override pisa el slot de mismo nombre; nuevo metodo
        // se anyade al final.
        auto new_vtbl = std::make_unique<MethodInfo *[]>(new_count);
        for (size_t i = 0; i < new_count; ++i) new_vtbl[i] = &new_arr[i];

        cls->methods      = new_arr.get();
        cls->method_count = new_count;
        cls->vtable       = new_vtbl.get();
        cls->vtable_size  = new_count;
        method_arrays_.push_back(std::move(new_arr));
        vtable_arrays_.push_back(std::move(new_vtbl));
        rebuild_method_lookup(cls);
        return true;
    }

    // ---------------------------------------------------------------------
    //  find_class / find_field / find_method
    // ---------------------------------------------------------------------

    ClassInfo *ClassRegistry::find_class(const std::string &name) const {
        auto it = by_name_.find(name);
        return (it == by_name_.end()) ? nullptr : it->second;
    }

    FieldInfo *ClassRegistry::find_field(ClassInfo *cls, const char *name, size_t name_len) {
        if (!cls) return nullptr;
        const uint32_t idx = lookup_slot(cls->field_lookup_table,
                                         cls->field_lookup_mask, name, name_len);
        if (idx == UINT32_MAX) return nullptr;
        // index puede apuntar a fields[] o a static_fields[].  Por
        // convencion del builder, los static_fields tienen indice
        // >= field_count.
        if (idx < cls->field_count) return &cls->fields[idx];
        const uint32_t static_idx = idx - static_cast<uint32_t>(cls->field_count);
        if (static_idx < cls->static_field_count) return &cls->static_fields[static_idx];
        return nullptr;
    }

    MethodInfo *ClassRegistry::find_method(ClassInfo *cls, const char *name, size_t name_len) {
        if (!cls) return nullptr;
        const uint32_t idx = lookup_slot(cls->method_lookup_table,
                                         cls->method_lookup_mask, name, name_len);
        if (idx == UINT32_MAX) return nullptr;
        if (idx >= cls->method_count) return nullptr;
        return &cls->methods[idx];
    }

    // ---------------------------------------------------------------------
    //  Dispatch de interfaz (itables) -- construccion lazy + relleno por nombre
    //
    //  Las interfaces NO registran sus metodos en runtime (sus ClassInfo no
    //  tienen methods/vtable), asi que la itable NO se construye iterando
    //  iface->methods.  En su lugar se DIMENSIONA por @p count (que el frontend
    //  conoce = nº de metodos de la interfaz) con todos los slots a nullptr, y
    //  cada slot se RELLENA lazy por nombre (resolve_itable_method) la primera
    //  vez que se despacha ese (clase, indice).  Tras el warmup el dispatch es
    //  un indice puro (sin lookup por nombre).
    // ---------------------------------------------------------------------
    ItableEntry *ClassRegistry::get_or_build_itable(ClassInfo *cls, ClassInfo *iface,
                                                    uint32_t count) {
        if (!cls || !iface) return nullptr;

        // Fast path (lock-free): el array de itables es NULL-terminado
        // (ultima entry con iface==0).  Leemos un unico puntero (8B alineado
        // = atomico en x86/ARM64) y recorremos hasta el terminador.  Un lector
        // que tenga el puntero "viejo" (antes de un build concurrente) escanea
        // el array viejo: si la iface no esta, caera al slow path y la hallara
        // bajo lock (los arrays viejos se retienen en los pools).
        if (ItableEntry *arr = cls->itables) {
            for (ItableEntry *e = arr; e->iface != nullptr; ++e) {
                if (e->iface == iface) return e;
            }
        }

        // Slow path: construir bajo lock con re-check (otro hilo pudo haberla
        // construido entre el fast path y la adquisicion del lock).
        std::lock_guard<std::mutex> lk(itable_mutex_);
        if (ItableEntry *arr = cls->itables) {
            for (ItableEntry *e = arr; e->iface != nullptr; ++e) {
                if (e->iface == iface) return e;
            }
        }

        // Crear el array methods[] dimensionado a count, TODO a nullptr
        // (make_unique value-initializa).  Los slots se rellenan lazy por
        // nombre en resolve_itable_method.
        MethodInfo **methods = nullptr;
        if (count > 0) {
            auto mbuf = std::make_unique<MethodInfo *[]>(count);
            methods = mbuf.get();
            itable_method_arrays_.push_back(std::move(mbuf));
        }

        // Realloc del array de itables: copia de las entradas previas + la
        // nueva + el terminador NULL.  make_unique value-initializa (zero) las
        // entradas, asi que el terminador queda {nullptr,nullptr,0,0}.
        const uint32_t old_n = cls->itable_count;
        auto nbuf = std::make_unique<ItableEntry[]>(old_n + 2); // +1 nueva +1 terminador
        for (uint32_t i = 0; i < old_n; ++i) nbuf[i] = cls->itables[i];
        nbuf[old_n].iface   = iface;
        nbuf[old_n].methods = methods;
        nbuf[old_n].count   = count;
        nbuf[old_n]._pad    = 0;
        // nbuf[old_n + 1] ya es el terminador {0,0,0,0} por value-init.

        ItableEntry *result = &nbuf[old_n];
        ItableEntry *pub    = nbuf.get();
        itable_arrays_.push_back(std::move(nbuf));

        // Publicar: el contenido del array ya esta escrito; barrera release +
        // store del puntero (RCU-style).  El lector consume via dependencia de
        // direccion sobre el puntero cargado (ve el contenido del array).
        std::atomic_thread_fence(std::memory_order_release);
        cls->itables      = pub;
        cls->itable_count = old_n + 1; // metadata (el scan hot usa el terminador)
        return result;
    }

    MethodInfo *ClassRegistry::resolve_itable_method(ClassInfo *cls, ClassInfo *iface,
                                                     uint32_t count, uint32_t method_index,
                                                     const char *method_name,
                                                     size_t name_len) {
        ItableEntry *e = get_or_build_itable(cls, iface, count);
        if (!e || method_index >= e->count || !e->methods) {
            // Indice fuera de rango (no deberia ocurrir para programas
            // bien-formados): fallback directo por nombre.
            return find_method(cls, method_name, name_len);
        }
        // Slot ya resuelto -> indice puro (fast path comun tras el warmup).
        MethodInfo *m = e->methods[method_index];
        if (m) return m;
        // Primer dispatch de este (clase, indice): resolver por nombre y
        // cachear.  Escritura idempotente (mismo MethodInfo* desde cualquier
        // hilo) de 8B alineada -> thread-safe sin lock.
        m = find_method(cls, method_name, name_len);
        e->methods[method_index] = m;
        return m;
    }

} // namespace loader
