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
 * @file gc_heap.cpp
 * @brief Implementacion del heap generacional con GC tri-color de VestaVM.
 *
 * Implementa @c gc::GCHeap: asignacion en nursery, promocion a old-gen,
 * ciclos de recoleccion mayor/menor, write-barrier y las fases del algoritmo
 * tri-color mark-and-sweep (blanco/gris/negro).
 */
#include "gc/gc_heap.h"
// acceso al ProcessVM owner para leer stack/regs durante GC.
// Solo en el .cpp (no en gc_heap.h) para evitar incluir gc_heap.h en
// proceso_runtime.h (gc_heap.h ya esta en proceso_runtime.h).
#include "runtime/proceso_runtime.h"
#include "loader/oop_types.h"

#include <cstring>
#include <cassert>

namespace gc {

    // -------------------------------------------------------------------------
    // Constructor / destructor
    // -------------------------------------------------------------------------

    GcHeap::GcHeap(vm::ArenaManager &arena_mgr, size_t nursery_bytes, size_t old_threshold)
        : arena_mgr_(arena_mgr), nursery_size_(nursery_bytes), old_threshold_(old_threshold)
    {
        nursery_arena_id_ = arena_mgr_.create_arena(nursery_bytes,
                                                     vm::MemPerm::READ | vm::MemPerm::WRITE);
        const vm::Arena *a = arena_mgr_.get_arena(nursery_arena_id_);
        assert(a && a->ptr && "GcHeap: no se pudo crear la Nursery");

        nursery_base_ = static_cast<uint8_t *>(a->ptr);
        nursery_bump_ = nursery_base_;
        nursery_end_  = nursery_base_ + nursery_bytes;
    }

    GcHeap::~GcHeap() {
        for (auto &b : old_blocks_)
            arena_mgr_.free_arena(b.arena_id);
        old_blocks_.clear();

        arena_mgr_.free_arena(nursery_arena_id_);
        nursery_base_ = nursery_bump_ = nursery_end_ = nullptr;
    }

    // -------------------------------------------------------------------------
    // Gestion de handles
    // -------------------------------------------------------------------------

    GcHandle GcHeap::new_handle(uint8_t *addr) {
        // GC_NULL_HANDLE = 0 colisiona con el indice 0 del array.
        // Reservamos handles_[0] como sentinela "no usado" en la primera
        // llamada para que ningun handle valido sea 0.  Sin esto, el
        // primer alloc del proceso retornaba 0, indistinguible de
        // "alloc fallo" -- causa real de strmake retornando NULL_HANDLE
        // en programas Vex donde el primer use de string era el primer
        // alloc del GC heap (caso comun).
        if (handles_.empty()) {
            handles_.push_back({ nullptr, false });   // sentinela inalcanzable
        }
        GcHandle h;
        if (!free_handles_.empty()) {
            h = free_handles_.back();
            free_handles_.pop_back();
            handles_[h] = { addr, true };
        } else {
            h = static_cast<GcHandle>(handles_.size());
            handles_.push_back({ addr, true });
        }
        // Registrar el host pointer del payload (no del GcHeader) en el
        // mapa inverso.  Permite handle_for_ptr O(1) sin escanear handles_.
        if (addr != nullptr) {
            const uint8_t *payload = addr + sizeof(GcHeader);
            ptr_to_handle_[payload] = h;
        }
        return h;
    }

    void GcHeap::release_handle(GcHandle h) {
        if (h >= handles_.size()) return;
        // si el handle esta pinnado externamente (external_refs > 0),
        // NO liberamos: alguien tiene una referencia externa (ej. ArrayList
        // <string> del plugin) y necesita el objeto vivo.  El mismo handle
        // sera liberado automaticamente cuando el ultimo gc_release lo
        // saque del map.
        auto it_ext = external_refs_.find(h);
        if (it_ext != external_refs_.end() && it_ext->second > 0) {
            return;
        }
        // Limpiar el mapa inverso ANTES de invalidar handles_[h].addr.
        // Necesitamos el addr para localizar el payload pointer que
        // registramos en new_handle/do_evacuate.
        const uint8_t *old_addr = handles_[h].addr;
        if (old_addr != nullptr) {
            ptr_to_handle_.erase(old_addr + sizeof(GcHeader));
        }
        handles_[h] = { nullptr, false };
        free_handles_.push_back(h);
    }

    GcHandle GcHeap::handle_for_ptr(const uint8_t *host_payload_ptr) const noexcept {
        // Lookup directo O(1) amortizado.  El mapa se mantiene en
        // new_handle/release_handle/do_evacuate, asi que aqui no hay
        // escaneo lineal: una sola sonda en la tabla hash.
        if (host_payload_ptr == nullptr) return GC_NULL_HANDLE;
        auto it = ptr_to_handle_.find(host_payload_ptr);
        if (it == ptr_to_handle_.end()) return GC_NULL_HANDLE;
        return it->second;
    }

    // -------------------------------------------------------------------------
    // alloc
    // -------------------------------------------------------------------------

    GcHandle GcHeap::alloc(size_t size) {
        constexpr size_t ALIGN = 8;
        // total = bytes que necesita el slot para cubrir cabecera + payload
        // pedido + padding hasta multiplo de 8.
        const size_t total = (sizeof(GcHeader) + size + ALIGN - 1) & ~(ALIGN - 1);

        // Lambda que inicializa un slot ya reservado.  Recibe el puntero
        // al GcHeader y el tamano REAL del slot (slot_total).
        //
        // Convencion de hdr->size:
        //   - Nursery: hdr->size = @p size (user payload).  Conserva la
        //     semantica clasica.  El slot fue alocado con bump pointer
        //     exacto (slot_total = round_up(header + size, 8)), no hay
        //     ambigueedad: SWEEP de OldGen no toca nursery, asi que da igual.
        //   - OldGen: hdr->size = slot_total - header (payload del slot,
        //     incluyendo padding hasta el size class).  Necesario para que
        //     al re-free el slot encaje exactamente en su size class via
        //     size_class_ceil(round_up(header + hdr->size, 8)).
        //
        // El zero-init memset solo cubre los bytes que pidio el usuario;
        // el padding interno queda en el estado previo (cero si bump fresh,
        // basura si reciclado de free list, pero inaccesible si el usuario
        // respeta @p size).
        auto init_obj = [&](uint8_t *raw, GcGen gen, size_t slot_total) -> GcHandle {
            auto *hdr  = reinterpret_cast<GcHeader *>(raw);
            if (gen == GcGen::OLD) {
                hdr->size = static_cast<uint32_t>(slot_total - sizeof(GcHeader));
            } else {
                hdr->size = static_cast<uint32_t>(size);
            }
            hdr->color = GcColor::WHITE;
            hdr->gen   = gen;
            std::memset(raw + sizeof(GcHeader), 0, size);

            stats_.alloc_count++;
            stats_.alloc_bytes += size;          // bytes utiles pedidos
            size_t nu = nursery_used();
            if (nu > stats_.peak_nursery) stats_.peak_nursery = nu;
            if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;

            return new_handle(raw);
        };

        // Fast-path: espacio en Nursery (bump pointer, sin redondeo a class).
        // La Nursery se evacua entera por minor_gc; no hay reuso individual
        // de slots, asi que no necesitamos size classes alli.
        if (nursery_bump_ + total <= nursery_end_) {
            uint8_t *raw   = nursery_bump_;
            nursery_bump_ += total;
            return init_obj(raw, GcGen::YOUNG, total);
        }

        // Nursery llena: minor GC y reintentar.
        minor_gc();

        if (nursery_bump_ + total <= nursery_end_) {
            uint8_t *raw   = nursery_bump_;
            nursery_bump_ += total;
            return init_obj(raw, GcGen::YOUNG, total);
        }

        // Sigue sin espacio: asignar directo en OldGen via free list o bump.
        // alloc_in_old puede devolver un slot >= total (redondeo al size class).
        size_t actual_total = 0;
        uint8_t *raw = alloc_in_old_with_total(total, actual_total);
        if (!raw) return GC_NULL_HANDLE;
        return init_obj(raw, GcGen::OLD, actual_total);
    }

    // -------------------------------------------------------------------------
    // alloc_pinned - aloca directo en OldGen (non-moving) para objetos cuyo
    // host_ptr al payload debe permanecer estable a traves del GC.
    //
    // Caso de uso primario: StringObject.  El bytecode obtiene el host_ptr
    // del buffer de datos via STRRAW para pasarlo a FFI / print / interpolacion.
    // Si el StringObject vive en young y un GC menor lo evacua a old, el
    // host_ptr queda dangling.  Allocando directo en old desde el principio,
    // el host_ptr es estable mientras el handle viva.
    //
    // Penalizacion: Old llena mas rapido y dispara major_gc antes.  Para
    // strings tipicos (cortos, vida corta-media) esto es aceptable.
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // Stack scanning conservativo con interior scan en OldGen.
    //
    // Fundamento: el GC anterior consideraba root a TODO handle vivo en la
    // HandleTable.  Sin liberacion explicita (drop / RAII), los handles se
    // acumulaban indefinidamente -> el major_gc nunca colectaba.
    //
    // Nuevo modelo: el GC escanea el stack del proceso + GP regs + external_refs
    // + pending_alloc_root_ buscando handles y host_ptrs a objetos GC.  Solo
    // los descubiertos son roots; los demas se barren.
    //
    // Los filtros descartan rapido los uint64_t que claramente NO son refs
    // (cero, escalares pequenos, valores fuera de rango handle) sin pagar
    // hash lookup.  Para los candidatos, lookup O(1) en handles_ o
    // ptr_to_handle_ + interior scan en OldGen para casos como STRRAW que
    // retorna ptr al data[] interno de un StringObject.
    // -------------------------------------------------------------------------

    /// Helper: dado un bloque OldGen y un host_ptr v dentro, encuentra el
    /// ObjectHeader que lo contiene recorriendo desde block.ptr siguiendo
    /// los headers consecutivos.  Retorna nullptr si v no cae en un objeto
    /// vivo (entre objetos, en zona DEAD, o fuera de bump_offset).
    ///
    /// Coste: O(N_objects_en_bloque).  Solo se llama si v ya cayo dentro
    /// del rango [block.ptr, block.ptr + block.bump_offset).
    static GcHeader *find_containing_header(uint8_t *block_start,
                                            size_t bump_offset,
                                            uint8_t *target_ptr) {
        uint8_t *cursor = block_start;
        uint8_t *end    = block_start + bump_offset;
        while (cursor + sizeof(GcHeader) <= end) {
            auto *hdr = reinterpret_cast<GcHeader *>(cursor);
            if (hdr->size == 0) break;
            const size_t total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
            uint8_t *slot_end = cursor + total;
            if (target_ptr >= cursor && target_ptr < slot_end) {
                // v cae dentro de este slot.
                if (hdr->color == GcColor::DEAD) return nullptr;
                return hdr;
            }
            cursor = slot_end;
        }
        return nullptr;
    }

    void GcHeap::scan_stack_roots(uint64_t rsp,
                                   uint64_t stack_high,
                                   const uint64_t regs[16],
                                   vm::VirtualMemory &vm_mem,
                                   std::vector<GcHandle> &worklist)
    {
        // Helper local para procesar un uint64_t candidato a referencia.
        // Filtros rapidos primero, lookup despues.  Marca BLACK + worklist
        // si encuentra un handle vivo correspondiente.
        auto process_value = [&](uint64_t v) {
            // Filtros rapidos: descartan >90% de slots sin hash lookup.
            if (v == 0) return;
            if (v < 256) return;        // escalar pequeno (loop counters, flags)

            // 1. Es un GcHandle directo?  Marcamos BLACK tanto YOUNG como
            //    OLD: minor_gc usa el flag para decidir que evacuar; major_gc
            //    solo barre OLD pero el flag en YOUNG no estorba (se descarta
            //    al reset nursery_bump al final del minor).
            if (v < handles_.size()) {
                const GcHandle h = static_cast<GcHandle>(v);
                if (handles_[h].live && handles_[h].addr) {
                    auto *hdr = reinterpret_cast<GcHeader *>(handles_[h].addr);
                    if (hdr->color == GcColor::WHITE) {
                        hdr->color = GcColor::BLACK;
                        worklist.push_back(h);
                    }
                    return;
                }
            }

            // 2. Es un host_ptr al payload start de un objeto GC?
            //    (resultado de gcderef en CLASS instances o de strraw cuando
            //    apunta al inicio del StringObject).
            auto *ptr = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(v));
            auto it = ptr_to_handle_.find(ptr);
            if (it != ptr_to_handle_.end()) {
                const GcHandle h = it->second;
                if (h < handles_.size() && handles_[h].live && handles_[h].addr) {
                    auto *hdr = reinterpret_cast<GcHeader *>(handles_[h].addr);
                    if (hdr->color == GcColor::WHITE) {
                        hdr->color = GcColor::BLACK;
                        worklist.push_back(h);
                    }
                }
                return;
            }

            // 3a. Interior scan en OldGen: el ptr puede caer DENTRO del
            //     payload de un objeto OldGen sin ser el inicio.  Caso
            //     clasico: STRRAW retorna data[] del StringObject que esta
            //     en offset 40 del payload start.
            for (auto &block : old_blocks_) {
                uint8_t *block_end = block.ptr + block.bump_offset;
                if (ptr < block.ptr || ptr >= block_end) continue;
                // v cae en este bloque -> encontrar el header contenedor.
                GcHeader *hdr = find_containing_header(block.ptr,
                                                       block.bump_offset,
                                                       ptr);
                if (hdr == nullptr) return;          // entre objetos / DEAD
                if (hdr->color != GcColor::WHITE) return; // ya BLACK o DEAD
                // Buscar el handle correspondiente al header.
                uint8_t *payload = reinterpret_cast<uint8_t *>(hdr) + sizeof(GcHeader);
                auto it2 = ptr_to_handle_.find(payload);
                if (it2 == ptr_to_handle_.end()) return;
                const GcHandle h = it2->second;
                if (h >= handles_.size() || !handles_[h].live) return;
                hdr->color = GcColor::BLACK;
                worklist.push_back(h);
                return;
            }

            // 3b. Interior scan en Nursery: igual logica para objetos YOUNG
            //     que aun no se han evacuado.  Sin esto, un host_ptr derivado
            //     (e.g. data[] de StringObject recien creado) no marcaria el
            //     StringObject -> minor_gc lo descartaria.
            if (ptr >= nursery_base_ && ptr < nursery_bump_) {
                uint8_t *cursor = nursery_base_;
                while (cursor + sizeof(GcHeader) <= nursery_bump_) {
                    auto *hdr = reinterpret_cast<GcHeader *>(cursor);
                    if (hdr->size == 0) break;
                    const size_t total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
                    uint8_t *slot_end = cursor + total;
                    if (ptr >= cursor && ptr < slot_end) {
                        if (hdr->color != GcColor::WHITE) return;
                        uint8_t *payload = cursor + sizeof(GcHeader);
                        auto it3 = ptr_to_handle_.find(payload);
                        if (it3 == ptr_to_handle_.end()) return;
                        const GcHandle h = it3->second;
                        if (h >= handles_.size() || !handles_[h].live) return;
                        hdr->color = GcColor::BLACK;
                        worklist.push_back(h);
                        return;
                    }
                    cursor = slot_end;
                }
            }
            // Si no cae en ningun bloque, ignorar (escalar arbitrario).
        };

        // Escanear el stack del proceso: [rsp, stack_high) en pasos de 8 bytes.
        // Las direcciones son virtuales del proceso VM, leemos via vm_mem.
        // Limitamos a un rango razonable para evitar reads excesivos si el
        // stack_high sale corrupto.
        if (rsp < stack_high && (stack_high - rsp) < (16 * 1024 * 1024)) {
            for (uint64_t addr = rsp; addr + 8 <= stack_high; addr += 8) {
                const uint64_t v = vm_mem.read_u64(addr);
                process_value(v);
            }
        }

        // Escanear los GP regs R0..R15.  Cada reg puede contener un handle
        // o un host_ptr.  R13/R14/R15 son scratch del regalloc pero pueden
        // contener valores temporales que sean refs validas en este momento;
        // mejor escanearlos para no perder roots.
        for (int i = 0; i < 16; ++i) {
            process_value(regs[i]);
        }

        // pending_alloc_root_: handle en construccion durante un alloc que
        // disparo GC recursivo.  Lo marcamos como root para que no sea
        // barrido antes de que el alloc retorne y el bytecode lo guarde
        // en stack/regs.
        if (pending_alloc_root_ != GC_NULL_HANDLE
         && pending_alloc_root_ < handles_.size()
         && handles_[pending_alloc_root_].live
         && handles_[pending_alloc_root_].addr) {
            auto *hdr = reinterpret_cast<GcHeader *>(handles_[pending_alloc_root_].addr);
            if (hdr->gen == GcGen::OLD && hdr->color == GcColor::WHITE) {
                hdr->color = GcColor::BLACK;
                worklist.push_back(pending_alloc_root_);
            }
        }
    }

    GcHandle GcHeap::alloc_pinned(size_t size) {
        const size_t total = (sizeof(GcHeader) + size + 7) & ~7ULL;
        size_t actual_total = 0;
        uint8_t *raw = alloc_in_old_with_total(total, actual_total);
        if (!raw) return GC_NULL_HANDLE;
        // Inicializacion del header / contadores: equivalente al lambda
        // init_obj de @c alloc, pero inlineado porque la lambda es local
        // a aquel metodo.  Convencion OldGen: hdr->size = payload del
        // slot incluyendo padding hasta size class (necesario para que
        // free_in_old encuentre la free list correcta).
        auto *hdr = reinterpret_cast<GcHeader *>(raw);
        hdr->size  = static_cast<uint32_t>(actual_total - sizeof(GcHeader));
        hdr->color = GcColor::WHITE;
        hdr->gen   = GcGen::OLD;
        std::memset(raw + sizeof(GcHeader), 0, size);
        stats_.alloc_count++;
        stats_.alloc_bytes += size;
        if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;
        return new_handle(raw);
    }

    // -------------------------------------------------------------------------
    // deref
    // -------------------------------------------------------------------------

    uint8_t *GcHeap::deref(GcHandle h) {
        if (h >= handles_.size() || !handles_[h].live) return nullptr;
        return handles_[h].addr + sizeof(GcHeader);
    }

    // -------------------------------------------------------------------------
    // write_barrier  - O(1) con unordered_set
    // -------------------------------------------------------------------------

    void GcHeap::write_barrier(GcHandle old_handle) {
        remembered_set_.insert(old_handle);
    }

    // -------------------------------------------------------------------------
    // External refcount API.
    //
    // El GcHeap es per-process y NO multi-thread (uso exclusivo del thread
    // owner del proceso, garantizado por el scheduler).  Por tanto, no hay
    // necesidad de atomics ni locks en estas APIs: el plugin nativo se
    // ejecuta en el mismo thread que esta procesando bytecode del proceso.
    // Si futuro se introduce un GC concurrente o pre-emptive, deberian
    // anadirse atomics y/o locks aqui.
    // -------------------------------------------------------------------------

    void GcHeap::gc_addref(GcHandle h) {
        if (h == GC_NULL_HANDLE) return;
        // Lookup-or-insert con valor inicial 0, despues incrementar.
        // operator[] inserta automaticamente si no existe.
        external_refs_[h] += 1;
    }

    void GcHeap::gc_release(GcHandle h) {
        if (h == GC_NULL_HANDLE) return;
        auto it = external_refs_.find(h);
        if (it == external_refs_.end()) return; // no estaba pinnado: no-op
        if (it->second > 1) {
            it->second -= 1;
        } else {
            // Al llegar a 0 eliminamos la entrada para que el bucket se libere
            // y el mark phase no malgaste tiempo iterando handles ya liberados.
            external_refs_.erase(it);
        }
    }

    // -------------------------------------------------------------------------
    // do_evacuate - nucleo de evacuacion sin comprobacion de liveness
    // -------------------------------------------------------------------------

    void GcHeap::do_evacuate(GcHandle h) {
        uint8_t *src_raw = handles_[h].addr;
        if (!src_raw) return;

        auto *hdr = reinterpret_cast<GcHeader *>(src_raw);
        if (hdr->color == GcColor::BLACK || hdr->gen == GcGen::OLD) return;

        const size_t total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
        // (iv) alloc_in_old_with_total reporta el slot REAL en OldGen
        // (puede ser >= total por redondeo a size class).  Actualizamos
        // dst_hdr->size con el slot real para que al re-free el slot
        // encaje exactamente en su size class.
        size_t   actual_total = 0;
        uint8_t *dst_raw      = alloc_in_old_with_total(total, actual_total);
        if (!dst_raw) return;

        // Copiamos solo `total` bytes (el slot fuente); los bytes extras del
        // slot destino quedan inaccesibles para el usuario (padding interno).
        std::memcpy(dst_raw, src_raw, total);

        auto *dst_hdr  = reinterpret_cast<GcHeader *>(dst_raw);
        dst_hdr->gen   = GcGen::OLD;
        dst_hdr->color = GcColor::BLACK;
        // Actualizar size al payload real del slot OldGen (>= payload original).
        dst_hdr->size  = static_cast<uint32_t>(actual_total - sizeof(GcHeader));

        handles_[h].addr = dst_raw;

        // Mover la entrada del mapa inverso payload_ptr -> handle.  El
        // payload anterior se marcara como forward pointer y dejara de
        // ser un puntero valido a un objeto vivo, asi que lo retiramos.
        ptr_to_handle_.erase(src_raw + sizeof(GcHeader));
        ptr_to_handle_[dst_raw + sizeof(GcHeader)] = h;

        // Forward pointer en payload original para detectar doble evacuacion
        uint8_t *src_payload = src_raw + sizeof(GcHeader);
        std::memcpy(src_payload, &dst_raw, sizeof(void *));
        hdr->color = GcColor::BLACK;

        stats_.promoted_count++;
        stats_.promoted_bytes += hdr->size;
        if (old_used_ > stats_.peak_old) stats_.peak_old = old_used_;
    }

    void GcHeap::evacuate_object(GcHandle h) {
        if (h >= handles_.size() || !handles_[h].addr) return;
        do_evacuate(h);
    }

    // -------------------------------------------------------------------------
    // scan_young_refs - escanea payload buscando handles VIVOS que apunten a YOUNG
    // -------------------------------------------------------------------------

    void GcHeap::scan_young_refs(GcHandle h, std::vector<GcHandle>& worklist) {
        if (!handles_[h].addr) return;

        auto    *hdr     = reinterpret_cast<GcHeader *>(handles_[h].addr);
        uint8_t *payload = handles_[h].addr + sizeof(GcHeader);
        size_t   sz      = hdr->size;

        for (size_t off = 0; off + sizeof(GcHandle) <= sz; off += sizeof(GcHandle)) {
            GcHandle ref;
            std::memcpy(&ref, payload + off, sizeof(GcHandle));

            if (ref == GC_NULL_HANDLE || ref >= static_cast<GcHandle>(handles_.size())) continue;
            // Solo seguir handles vivos para evitar falsos positivos con datos numericos
            if (!handles_[ref].live || !handles_[ref].addr) continue;

            uint8_t *ref_raw = handles_[ref].addr;
            if (ref_raw < nursery_base_ || ref_raw >= nursery_end_) continue;

            auto *ref_hdr = reinterpret_cast<GcHeader *>(ref_raw);
            if (ref_hdr->color == GcColor::BLACK) continue;

            do_evacuate(ref);
            worklist.push_back(ref);
        }
    }

    // -------------------------------------------------------------------------
    // mark_reachable - BFS sobre handles VIVOS embebidos en payloads OLD
    // -------------------------------------------------------------------------

    void GcHeap::mark_reachable(GcHandle h, std::vector<GcHandle>& worklist) {
        if (!handles_[h].addr) return;

        auto    *hdr     = reinterpret_cast<GcHeader *>(handles_[h].addr);
        uint8_t *payload = handles_[h].addr + sizeof(GcHeader);
        size_t   sz      = hdr->size;

        for (size_t off = 0; off + sizeof(GcHandle) <= sz; off += sizeof(GcHandle)) {
            GcHandle ref;
            std::memcpy(&ref, payload + off, sizeof(GcHandle));

            if (ref == GC_NULL_HANDLE || ref >= static_cast<GcHandle>(handles_.size())) continue;
            // Solo seguir handles vivos: un handle soltado con drop() no mantiene
            // vivo al objeto aunque su valor este embebido en el payload
            if (!handles_[ref].live || !handles_[ref].addr) continue;

            auto *ref_hdr = reinterpret_cast<GcHeader *>(handles_[ref].addr);
            if (ref_hdr->gen != GcGen::OLD || ref_hdr->color != GcColor::WHITE) continue;

            ref_hdr->color = GcColor::BLACK;
            worklist.push_back(ref);
        }
    }

    // -------------------------------------------------------------------------
    // Minor GC - Cheney-style
    // -------------------------------------------------------------------------

    void GcHeap::minor_gc() {
        stats_.minor_gc_count++;

        // usar scan_stack_roots tambien en minor_gc para
        // evitar evacuar objetos YOUNG que NO son alcanzables desde stack
        // /regs/external_refs.  Sin esto, todos los handles live se
        // evacuaban a OldGen indiscriminadamente -> Old crecia rapido
        // -> major_gc se disparaba constantemente.
        //
        // Pre-mark: poner WHITE todos los YOUNG live (color por defecto
        // tras alloc).  Luego scan_stack_roots marca BLACK los alcanzables
        // (tanto YOUNG como OLD; OLD se ignora porque el sweep aqui solo
        // procesa young).  Solo evacuamos los BLACK.
        for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
            if (!handles_[h].live || !handles_[h].addr) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->gen == GcGen::YOUNG)
                hdr->color = GcColor::WHITE;
        }

        std::vector<GcHandle> worklist;
        if (owner_proc_ != nullptr) {
            const uint64_t rsp        = owner_proc_->registers.stack_pointer.qword();
            const uint64_t stack_high = owner_proc_->stack_high;
            uint64_t regs[16];
            for (int i = 0; i < 16; ++i) {
                regs[i] = owner_proc_->registers.regs[i].qword();
            }
            scan_stack_roots(rsp, stack_high, regs, owner_proc_->vm_mem, worklist);
        } else {
            // Fallback: modelo previo (todos los handles live = root).
            for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
                if (!handles_[h].live || !handles_[h].addr) continue;
                uint8_t *raw = handles_[h].addr;
                if (raw < nursery_base_ || raw >= nursery_end_) continue;
                auto *hdr = reinterpret_cast<GcHeader *>(raw);
                if (hdr->gen == GcGen::YOUNG && hdr->color == GcColor::WHITE) {
                    hdr->color = GcColor::BLACK;
                    worklist.push_back(h);
                }
            }
        }

        // external_refs: handles pinnados por plugins nativos.
        for (const auto &kv : external_refs_) {
            const GcHandle h = kv.first;
            if (kv.second == 0) continue;
            if (h >= handles_.size()) continue;
            if (!handles_[h].live || !handles_[h].addr) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->color == GcColor::WHITE) {
                hdr->color = GcColor::BLACK;
                worklist.push_back(h);
            }
        }

        // pending_alloc_root_: handle en construccion.
        if (pending_alloc_root_ != GC_NULL_HANDLE
         && pending_alloc_root_ < handles_.size()
         && handles_[pending_alloc_root_].live
         && handles_[pending_alloc_root_].addr) {
            uint8_t *raw = handles_[pending_alloc_root_].addr;
            if (raw >= nursery_base_ && raw < nursery_end_) {
                auto *hdr = reinterpret_cast<GcHeader *>(raw);
                if (hdr->color == GcColor::WHITE) {
                    hdr->color = GcColor::BLACK;
                    worklist.push_back(pending_alloc_root_);
                }
            }
        }

        // Evacuar SOLO los YOUNG marcados BLACK (alcanzables).  Los WHITE
        // se descartan al resetear nursery_bump_ abajo.
        for (GcHandle h : worklist) {
            if (h >= handles_.size() || !handles_[h].live || !handles_[h].addr) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->gen == GcGen::YOUNG) {
                do_evacuate(h);
            }
        }

        // Raices adicionales del remembered_set: OLD objects con refs a YOUNG vivos
        // Cubre el caso: objeto OLD escribe ref YOUNG via GCWB y el objeto YOUNG
        // solo es alcanzable desde ese campo (sin handle propio en el bytecode)
        std::vector<GcHandle> rs_worklist;
        for (GcHandle old_h : remembered_set_)
            scan_young_refs(old_h, rs_worklist);

        // los handles YOUNG no evacuados (color WHITE tras
        // scan) deben liberarse: handles_[h].live = false + payload_ptr
        // borrado del ptr_to_handle_.  Sin esto, los handles quedan live
        // pero su addr apunta al nursery que se va a resetear -> dangling.
        for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
            if (!handles_[h].live || !handles_[h].addr) continue;
            uint8_t *raw = handles_[h].addr;
            if (raw < nursery_base_ || raw >= nursery_end_) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(raw);
            if (hdr->gen == GcGen::YOUNG && hdr->color == GcColor::WHITE) {
                // No evacuado: el objeto muere con el reset del nursery.
                ptr_to_handle_.erase(raw + sizeof(GcHeader));
                handles_[h].addr = nullptr;
                handles_[h].live = false;
                free_handles_.push_back(h);
            }
        }

        // Reset BLACK -> WHITE para los YOUNG no procesados (ninguno tras
        // el sweep arriba) y para mantener invariante post-minor.
        // (No necesario en realidad porque los WHITE ya murieron y los
        // evacuados ya estan en OLD con BLACK).
        remembered_set_.clear();
        nursery_bump_ = nursery_base_;

        if (old_used_ >= old_threshold_)
            major_gc();
    }

    // -------------------------------------------------------------------------
    // Major GC - mark-and-sweep tri-color transitivo sobre OldGen
    // -------------------------------------------------------------------------

    void GcHeap::major_gc() {
        stats_.major_gc_count++;

        // PRE-MARK: todos los objetos OldGen vivos (no DEAD) -> WHITE
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.size;
            while (cursor + sizeof(GcHeader) <= end) {
                auto *hdr = reinterpret_cast<GcHeader *>(cursor);
                if (hdr->size == 0) break;
                if (hdr->color != GcColor::DEAD)
                    hdr->color = GcColor::WHITE;
                cursor += (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;
            }
        }

        // MARK: stack scanning conservativo desde stack/regs
        // del proceso owner.  Reemplaza el modelo previo "todo handle live
        // = root" que nunca colectaba sin drop explicito.  Si owner_proc_
        // es nullptr (ej. tests sin ProcessVM), fallback al modelo previo.
        std::vector<GcHandle> worklist;
        if (owner_proc_ != nullptr) {
            // Leer stack ptr + stack high del proceso.  Estos son atributos
            // del ProcessVM expuestos publicamente (no requieren accessor).
            const uint64_t rsp        = owner_proc_->registers.stack_pointer.qword();
            const uint64_t stack_high = owner_proc_->stack_high;
            // Snapshot de R0..R15 (16 GP regs) para pasar al scan.
            uint64_t regs[16];
            for (int i = 0; i < 16; ++i) {
                regs[i] = owner_proc_->registers.regs[i].qword();
            }
            scan_stack_roots(rsp, stack_high, regs, owner_proc_->vm_mem, worklist);
            // Reset low-water-mark al rsp actual: tras el GC, los slots
            // por debajo del rsp actual ya no son alcanzables; el watermark
            // debe limitar el rango del proximo scan al rango realmente
            // activo desde ahora.
            owner_proc_->stack_low_water = rsp;
        } else {
            // Fallback: modelo previo (todo handle live = root).  Solo se
            // usa si no hay owner_proc_ asociado (no deberia ocurrir en
            // produccion, pero defensivo para tests / setups especiales).
            for (GcHandle h = 0; h < static_cast<GcHandle>(handles_.size()); ++h) {
                if (!handles_[h].live || !handles_[h].addr) continue;
                auto *hdr = reinterpret_cast<GcHeader *>(handles_[h].addr);
                if (hdr->gen == GcGen::OLD && hdr->color == GcColor::WHITE) {
                    hdr->color = GcColor::BLACK;
                    worklist.push_back(h);
                }
            }
        }

        // MARK roots externos: handles pinnados por estructuras nativas
        // (ArrayList<string> del plugin vesta_collections, etc.) se tratan
        // como roots vivos.  Iteramos external_refs_; si refcount > 0,
        // marcamos BLACK + agregamos al worklist para BFS transitivo.  Asi
        // un string almacenado solo en un slot del array nativo NO es
        // colectado durante el major_gc.
        for (const auto &kv : external_refs_) {
            const GcHandle h = kv.first;
            if (kv.second == 0) continue;  // refcount 0: deberia haberse limpiado
            if (h >= handles_.size()) continue; // handle stale (objeto ya freed)
            if (!handles_[h].live || !handles_[h].addr) continue;
            auto *hdr = reinterpret_cast<GcHeader *>(handles_[h].addr);
            if (hdr->gen == GcGen::OLD && hdr->color == GcColor::WHITE) {
                hdr->color = GcColor::BLACK;
                worklist.push_back(h);
            }
            // Si esta en YOUNG, el minor_gc se encarga (no se evacua si esta
            // en external_refs_ tampoco -- ver minor_gc adaptado abajo).
        }

        // BFS transitivo: seguir handles VIVOS embebidos en payloads
        for (size_t i = 0; i < worklist.size(); ++i)
            mark_reachable(worklist[i], worklist);

        // SWEEP: WHITE (sin raiz) -> DEAD, y reconstruir free lists.
        //
        // (iv) GC no-moving: tras el sweep, los slots DEAD se vuelven a
        // colocar en sus free lists segregadas para que el siguiente
        // alloc_in_old los reuse en O(1).  Limpiamos las free lists al
        // inicio del sweep y las repoblamos al recorrer cada slot DEAD.
        // De este modo el estado de las listas siempre refleja la realidad
        // post-sweep, sin entradas obsoletas.
        //
        // Solo se barren los bytes [block.ptr, block.ptr + block.bump_offset);
        // los bytes posteriores nunca se aloacron y no contienen GcHeaders
        // validos.
        freelist_clear();
        for (auto &block : old_blocks_) {
            uint8_t *cursor = block.ptr;
            uint8_t *end    = block.ptr + block.bump_offset;

            while (cursor + sizeof(GcHeader) <= end) {
                auto  *hdr   = reinterpret_cast<GcHeader *>(cursor);
                if (hdr->size == 0) break;

                const size_t total = (sizeof(GcHeader) + hdr->size + 7) & ~7ULL;

                if (hdr->color == GcColor::WHITE) {
                    stats_.freed_count++;
                    stats_.freed_bytes += total;          // total slot, no payload
                    old_used_ -= total;
                    hdr->color = GcColor::DEAD;
                }

                if (hdr->color == GcColor::DEAD) {
                    // Re-insertar en su free list (sea exact match o large).
                    freelist_push(cursor, total);
                }

                cursor += total;
            }
        }

        // WEAK SWEEP: anular referencias debiles a objetos recolectados
        // Si el handle apuntado por una WeakEntry esta muerto, poner target = GC_NULL_HANDLE
        for (auto &entry : weak_table_) {
            if (!entry.live) continue; // entrada ya liberada
            if (entry.target == GC_NULL_HANDLE) continue; // ya anulada

            // verificar si el objeto sigue vivo (color != DEAD) en la tabla de handles
            bool alive = false;
            if (entry.target < handles_.size()) {
                const auto &he = handles_[entry.target];
                if (he.addr != nullptr && he.live) {
                    // he.addr apunta al GcHeader del objeto; leer su color
                    const auto *ghdr = reinterpret_cast<const GcHeader *>(he.addr);
                    alive = (ghdr->color != GcColor::DEAD);
                }
            }
            if (!alive) entry.target = GC_NULL_HANDLE; // objeto muerto: anular referencia
        }
    }

    // -------------------------------------------------------------------------
    // (iv) GC no-moving en OldGen: free lists segregadas + bump per-block
    // -------------------------------------------------------------------------
    //
    // Diseno de allocator:
    //   - 16 size classes (16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512,
    //     768, 1024, 1536, 2048, 4096) cubren la mayoria de objetos Vesta.
    //   - Cada size class tiene una free list LIFO (singly-linked) de slots
    //     DEAD reusables.  El next pointer reusa los primeros 8 bytes del
    //     payload del slot (cero memoria extra).
    //   - Slots con total > 4096 bytes van a una free list general
    //     (large_free_list_) buscada con first-fit lineal.  Lista corta
    //     en la practica.
    //   - Al pop de free list: O(1) por size class.
    //   - Al miss en todas las free lists: bump pointer en algun OldBlock
    //     con espacio (probamos en orden, primer fit).
    //   - Al miss tambien en bumps: crear nuevo OldBlock (arena_mgr).
    //
    // Garantia: los objetos OldGen NO se mueven nunca tras la asignacion
    // inicial.  Sus host_ptrs son estables hasta que el sweep los marque
    // DEAD (lo que ocurre solo cuando el handle se libera y no quedan
    // referencias del programa al objeto).  Esto elimina el bug de
    // host_ptr stale tras un GC para todos los objetos OldGen.
    //
    // Trade-off: fragmentacion interna por redondeo a size class
    // (~10-20% tipico) a cambio de cero overhead per acceso a campo.
    // -------------------------------------------------------------------------

    size_t GcHeap::size_class_ceil(size_t total) noexcept {
        // Busqueda lineal sobre 16 entradas: la tabla cabe en 1 cache line
        // y el branch predictor aprende rapido.  Mas eficiente que std::lower_bound
        // por la baja cardinalidad y la simplicidad del codigo emitido.
        for (size_t i = 0; i < SMALL_CLASS_COUNT; ++i) {
            if (SMALL_CLASS_SIZES[i] >= total) return i;
        }
        return SMALL_CLASS_COUNT; // total > 4096: cae en la free list large
    }

    void GcHeap::freelist_clear() noexcept {
        // Solo limpiar los heads.  Los nodos viven embebidos en los slots
        // DEAD; al re-push sobreescribimos sus next pointers.  Los slots
        // que ya no son DEAD tras el sweep simplemente no se reinsertan.
        for (size_t i = 0; i < SMALL_CLASS_COUNT; ++i) {
            small_free_lists_[i] = nullptr;
        }
        large_free_list_.clear();
        stats_.old_freelist_bytes = 0;
    }

    void GcHeap::freelist_push(uint8_t *raw_header, size_t total) {
        // Pre-condicion: hdr->size del slot ya refleja el slot_total - header.
        // Calculamos size class basado en el total (que ya incluye header+payload+pad).
        const size_t cls = size_class_ceil(total);
        if (cls < SMALL_CLASS_COUNT && total >= SMALL_CLASS_SIZES[cls]) {
            // Slot exactamente del tamano del class (porque alloc redondea
            // siempre al class).  Si por algun motivo el slot es mas grande
            // del esperado, lo dejamos en su clase: al re-allocar se
            // devolvera con el tamano real registrado en hdr->size.
            auto *node = reinterpret_cast<FreeNode *>(raw_header + sizeof(GcHeader));
            node->next = small_free_lists_[cls];
            small_free_lists_[cls] = node;
            stats_.old_freelist_bytes += total;
        } else {
            // Slot grande o que no encaja en ningun small class: free list general.
            large_free_list_.push_back({ raw_header, total });
            stats_.old_freelist_bytes += total;
        }
    }

    uint8_t *GcHeap::alloc_in_old(size_t total_bytes) {
        size_t actual = 0;
        return alloc_in_old_with_total(total_bytes, actual);
    }

    uint8_t *GcHeap::alloc_in_old_with_total(size_t total_bytes,
                                              size_t &out_actual_total) {
        // 1. Free list segregada del size class apropiado.
        const size_t cls = size_class_ceil(total_bytes);
        if (cls < SMALL_CLASS_COUNT) {
            FreeNode *node = small_free_lists_[cls];
            if (node != nullptr) {
                small_free_lists_[cls] = node->next;
                // Recuperar el header del slot (el next pointer estaba en
                // payload[0..7], asi que el header esta justo antes).
                uint8_t *raw = reinterpret_cast<uint8_t *>(node) - sizeof(GcHeader);
                out_actual_total = SMALL_CLASS_SIZES[cls];
                old_used_ += out_actual_total;
                if (stats_.old_freelist_bytes >= out_actual_total) {
                    stats_.old_freelist_bytes -= out_actual_total;
                }
                stats_.old_alloc_freelist++;
                return raw;
            }
            // Si el class exacto esta vacio, intentar el siguiente size class
            // (slot mas grande del estrictamente necesario).  Limitamos a 2
            // buckets adicionales para no hacer searches largos: si llegamos
            // aqui es porque el patron de alloc no encaja con free lists
            // pre-existentes y caera al bump pointer rapidamente.
            for (size_t off = 1; off <= 2 && cls + off < SMALL_CLASS_COUNT; ++off) {
                FreeNode *n = small_free_lists_[cls + off];
                if (n != nullptr) {
                    small_free_lists_[cls + off] = n->next;
                    uint8_t *raw = reinterpret_cast<uint8_t *>(n) - sizeof(GcHeader);
                    out_actual_total = SMALL_CLASS_SIZES[cls + off];
                    old_used_ += out_actual_total;
                    if (stats_.old_freelist_bytes >= out_actual_total) {
                        stats_.old_freelist_bytes -= out_actual_total;
                    }
                    stats_.old_alloc_freelist++;
                    return raw;
                }
            }
        } else {
            // 2. Free list general para slots grandes (>4096): first-fit lineal.
            for (size_t i = 0; i < large_free_list_.size(); ++i) {
                if (large_free_list_[i].total >= total_bytes) {
                    uint8_t *raw         = large_free_list_[i].ptr;
                    out_actual_total     = large_free_list_[i].total;
                    // Erase swap-and-pop O(1) (no nos importa el orden).
                    large_free_list_[i]  = large_free_list_.back();
                    large_free_list_.pop_back();
                    old_used_ += out_actual_total;
                    if (stats_.old_freelist_bytes >= out_actual_total) {
                        stats_.old_freelist_bytes -= out_actual_total;
                    }
                    stats_.old_alloc_freelist++;
                    return raw;
                }
            }
        }

        // 3. Sin slot reusable: bump pointer en algun OldBlock con espacio.
        // Redondeamos al size class para que el slot creado sea reciclable
        // exactamente en su class al ser liberado mas tarde.
        const size_t alloc_total = (cls < SMALL_CLASS_COUNT)
                                    ? SMALL_CLASS_SIZES[cls]
                                    : ((total_bytes + 7) & ~7ULL);
        for (auto &block : old_blocks_) {
            const size_t available = block.size - block.bump_offset;
            if (available >= alloc_total) {
                uint8_t *raw       = block.ptr + block.bump_offset;
                block.bump_offset += alloc_total;
                out_actual_total   = alloc_total;
                old_used_                  += alloc_total;
                stats_.old_reserved_bytes  += alloc_total;
                stats_.old_alloc_bump++;
                return raw;
            }
        }

        // 4. Crear bloque nuevo.  Tamano: max(64KB, alloc_total*2) para
        // amortizar el overhead de la syscall y dejar espacio para futuros
        // alocs sin syscall.
        const size_t block_size = alloc_total < 64 * 1024 ? 64 * 1024 : alloc_total * 2;
        uint64_t         aid     = arena_mgr_.create_arena(
            block_size, vm::MemPerm::READ | vm::MemPerm::WRITE);
        const vm::Arena *a       = arena_mgr_.get_arena(aid);
        if (!a || !a->ptr) {
            out_actual_total = 0;
            return nullptr;
        }
        old_blocks_.push_back({
            static_cast<uint8_t *>(a->ptr),
            block_size,
            aid,
            alloc_total       // bump_offset arranca tras el primer slot
        });
        out_actual_total           = alloc_total;
        old_used_                 += alloc_total;
        stats_.old_reserved_bytes += alloc_total;
        stats_.old_alloc_newblock++;
        return static_cast<uint8_t *>(a->ptr);
    }

    // =========================================================================
    //  Tabla de referencias debiles
    // =========================================================================

    /**
     * @brief Registra un GcHandle en la tabla de referencias debiles.
     *
     * Busca primero un slot libre (live==false) para reutilizarlo.  Si no hay
     * ninguno disponible, inserta una nueva entrada al final.
     *
     * @param target GcHandle del objeto a observar debilmente.
     * @return Indice opaco uint32_t de la entrada en la tabla.
     */
    uint32_t GcHeap::alloc_weak(GcHandle target) {
        // reutilizar slots liberados antes de crecer la tabla
        for (uint32_t i = 0; i < static_cast<uint32_t>(weak_table_.size()); ++i) {
            if (!weak_table_[i].live) {
                weak_table_[i].target = target;
                weak_table_[i].live   = true;
                return i;
            }
        }
        // ningun slot libre: anadir al final de la tabla
        uint32_t idx = static_cast<uint32_t>(weak_table_.size());
        weak_table_.push_back({ target, true });
        return idx;
    }

    /**
     * @brief Resuelve un indice de weak ref al GcHandle subyacente.
     *
     * Si el objeto fue recolectado durante el ultimo major GC el campo target
     * habra sido puesto a GC_NULL_HANDLE por el barrido de weak refs.
     *
     * @param idx Indice opaco devuelto por alloc_weak().
     * @return GcHandle del objeto si sigue vivo; GC_NULL_HANDLE si fue recolectado
     *         o si el indice esta fuera de rango.
     */
    GcHandle GcHeap::deref_weak(uint32_t idx) const {
        if (idx >= static_cast<uint32_t>(weak_table_.size())) return GC_NULL_HANDLE;
        const auto &entry = weak_table_[idx];
        if (!entry.live) return GC_NULL_HANDLE; // slot liberado: referencia invalida
        return entry.target; // GC_NULL_HANDLE si el objeto fue recolectado, handle valido si no
    }

    /**
     * @brief Libera una entrada de la tabla de referencias debiles.
     *
     * Marca el slot como libre (live=false) para que pueda ser reutilizado.
     * Si idx esta fuera de rango o el slot ya estaba libre, no hace nada.
     *
     * @param idx Indice opaco devuelto por alloc_weak().
     */
    void GcHeap::free_weak(uint32_t idx) {
        if (idx >= static_cast<uint32_t>(weak_table_.size())) return;
        weak_table_[idx].live   = false;         // marcar como slot libre
        weak_table_[idx].target = GC_NULL_HANDLE; // limpiar el handle por seguridad
    }

    // -------------------------------------------------------------------------
    // Monitor / sincronizacion
    // -------------------------------------------------------------------------

    /**
     * @brief Intenta adquirir el monitor del objeto referenciado por @p h.
     *
     * Si owner_pid == 0 (libre): asigna el monitor a @p local_pid,
     * establece lock_depth = 1 y devuelve true.
     * Si owner_pid == local_pid (lock reentrante): incrementa lock_depth y
     * devuelve true.
     * Si lo posee otro proceso: devuelve false (el llamante debe bloquear).
     *
     * @param h         Handle del objeto cuyo monitor se quiere adquirir.
     * @param local_pid PID local del proceso solicitante.
     * @return true si el monitor fue adquirido o incrementado.
     */
    bool GcHeap::monitor_try_acquire(GcHandle h, uint32_t local_pid) {
        uint8_t *ptr = deref(h);
        if (ptr == nullptr) return false; // handle invalido: no se puede adquirir

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(ptr);

        if (hdr->owner_pid == 0) {
            hdr->owner_pid  = local_pid; // adquirir el monitor
            hdr->lock_depth = 1;
            return true;
        }
        if (hdr->owner_pid == local_pid) {
            hdr->lock_depth++; // lock reentrante: incrementar profundidad
            return true;
        }
        return false; // monitor ocupado por otro proceso
    }

    /**
     * @brief Libera el monitor del objeto referenciado por @p h.
     *
     * Decrementa lock_depth.  Cuando llega a 0 el monitor queda libre
     * (owner_pid = 0).  Extrae el primer proceso de la cola de espera
     * para despertarlo.
     *
     * @param h         Handle del objeto cuyo monitor se libera.
     * @param local_pid PID local del propietario actual (para validar).
     * @return PID codificado del siguiente proceso en cola (0 si vacia).
     */
    uint64_t GcHeap::monitor_release(GcHandle h, uint32_t local_pid) {
        uint8_t *ptr = deref(h);
        if (ptr == nullptr) return 0;

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(ptr);
        if (hdr->owner_pid != local_pid) return 0; // no es el propietario

        if (hdr->lock_depth > 1) {
            hdr->lock_depth--; // reducir nivel de reentrada sin liberar el monitor
            return 0;
        }

        // lock_depth llega a 0: liberar el monitor
        hdr->owner_pid  = 0;
        hdr->lock_depth = 0;

        return monitor_pop_waiter(h); // devolver el siguiente proceso a despertar
    }

    /**
     * @brief Anade un PID codificado a la cola de espera del monitor de @p h.
     *
     * @param h           Handle del objeto cuyo monitor tiene la cola.
     * @param encoded_pid PID codificado: (scheduler_id << 32) | local_pid.
     */
    void GcHeap::monitor_add_waiter(GcHandle h, uint64_t encoded_pid) {
        monitor_waiters_[h].push_back(encoded_pid); // insertar al final de la cola FIFO
    }

    /**
     * @brief Extrae y devuelve el primer PID de la cola de espera del monitor de @p h.
     *
     * @param h Handle del objeto cuya cola de espera se consulta.
     * @return PID codificado del primer proceso en cola, o 0 si la cola esta vacia.
     */
    uint64_t GcHeap::monitor_pop_waiter(GcHandle h) {
        auto it = monitor_waiters_.find(h);
        if (it == monitor_waiters_.end() || it->second.empty()) return 0;

        uint64_t pid = it->second.front(); // extraer el primero de la cola FIFO
        it->second.erase(it->second.begin());
        if (it->second.empty()) monitor_waiters_.erase(it); // limpiar entrada vacia
        return pid;
    }

    /**
     * @brief Extrae y devuelve todos los PID de la cola de espera del monitor de @p h.
     *
     * La cola queda vacia tras la llamada.
     *
     * @param h Handle del objeto cuya cola de espera se vacia.
     * @return Vector con todos los PID codificados.
     */
    std::vector<uint64_t> GcHeap::monitor_pop_all_waiters(GcHandle h) {
        auto it = monitor_waiters_.find(h);
        if (it == monitor_waiters_.end()) return {}; // cola inexistente: devolver vacio

        std::vector<uint64_t> result = std::move(it->second); // mover para evitar copia
        monitor_waiters_.erase(it); // limpiar la entrada
        return result;
    }

} // namespace gc
