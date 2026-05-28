/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file shared_heap.cpp
 * @brief Implementacion del slab allocator lock-free con growth dinamico.
 *
 * Ver @c include/gc/shared_heap.h para el contrato y el disenyo.
 *
 * Notas de implementacion
 * =======================
 * - **Memoria del slab**: @c vm::allocate_memory con permisos READ+WRITE.
 *   Una reserva por chunk; cada slab puede tener hasta
 *   @c SHARED_MAX_CHUNKS_PER_SLAB chunks lazy-allocados bajo demanda.
 *
 * - **Free list inicial**: tras la reserva del chunk, se construye la
 *   cadena intrusiva conectando los @c slots_per_chunk slots del chunk
 *   en orden secuencial.  Al crecer, los slots del chunk nuevo se enchufan
 *   al frente de la pila Treiber via un push masivo CAS.
 *
 * - **Growth lock-free**: cuando @c alloc encuentra free_head==0, hace
 *   CAS de 0 a 1 sobre @c growing.  El ganador aloca un chunk nuevo,
 *   thread sus slots al free list, y limpia el flag.  Los perdedores
 *   spinean con PAUSE hasta que el flag vuelva a 0, luego reintentan.
 *
 * - **Memory ordering**: load del @c free_head con @c acquire; CAS con
 *   @c acq_rel; store del @c next del nodo con @c relaxed (sincronizado
 *   por el CAS del head).
 *
 * - **Bound de retries**: < 5 en condiciones normales; bajo growth simultaneo
 *   puede llegar a 10-20 por la espera del flag.  Sin exponential backoff.
 *
 * - **No-op en free de puntero ajeno**: si el caller pasa un ptr que no
 *   esta en ningun chunk del heap, retornamos sin tocar nada.
 */

#include "gc/shared_heap.h"

#include "arena/arena.h"

#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#  define SHARED_HEAP_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define SHARED_HEAP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define SHARED_HEAP_LIKELY(x)   (x)
#  define SHARED_HEAP_UNLIKELY(x) (x)
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  if defined(_MSC_VER)
#    include <intrin.h>
#    define SHARED_HEAP_PAUSE() _mm_pause()
#  else
#    define SHARED_HEAP_PAUSE() __builtin_ia32_pause()
#  endif
#elif defined(__aarch64__) || defined(__arm__)
#  define SHARED_HEAP_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#  define SHARED_HEAP_PAUSE() ((void)0)
#endif

namespace gc {

    // ----------------------------------------------------------------------
    // Helpers internos: threading de un chunk nuevo al free list
    // ----------------------------------------------------------------------

    /**
     * @brief Construye la cadena intrusiva sobre el chunk @p base.
     *
     * Conecta los @p slots_per_chunk slots de @p slot_size bytes en orden
     * ascendente.  Devuelve (first, last) listos para hacer push masivo
     * a la pila Treiber: first->next ya esta seteado al siguiente del
     * chunk; el caller debe setear last->next al head viejo via CAS.
     */
    static void build_chunk_chain(uint8_t *base,
                                  uint32_t slot_size,
                                  uint32_t slots_per_chunk,
                                  SharedFreeNode **out_first,
                                  SharedFreeNode **out_last) noexcept {
        // Conectar slot i -> slot i+1 para i en [0, slots_per_chunk-1).
        for (uint32_t i = 0; i + 1 < slots_per_chunk; ++i) {
            SharedFreeNode *cur  = reinterpret_cast<SharedFreeNode *>(
                                       base + (size_t)i * slot_size);
            SharedFreeNode *next = reinterpret_cast<SharedFreeNode *>(
                                       base + (size_t)(i + 1) * slot_size);
            cur->next = next;
        }
        // Ultimo slot temporalmente apunta a null; el caller lo conectara
        // al head viejo via CAS antes de publicar el chunk.
        SharedFreeNode *last = reinterpret_cast<SharedFreeNode *>(
                                   base + (size_t)(slots_per_chunk - 1) * slot_size);
        last->next = nullptr;

        *out_first = reinterpret_cast<SharedFreeNode *>(base);
        *out_last  = last;
    }

    // ----------------------------------------------------------------------
    // Constructor / Destructor
    // ----------------------------------------------------------------------

    SharedHeap::SharedHeap() noexcept : init_ok_(true) {
        // Zero-init de los slabs: garantiza atomics inicializados a 0,
        // chunks_count=0, chunks[] base/end=nullptr.
        std::memset(&slabs_, 0, sizeof(slabs_));

        // Inicializar cada slab con su primer chunk.  Si CUALQUIER chunk
        // inicial falla, marcamos el heap como no-ok pero seguimos.
        for (uint32_t cls = 0; cls < SHARED_SIZE_CLASS_COUNT; ++cls) {
            if (!init_slab(cls)) {
                init_ok_ = false;
            }
        }
    }

    SharedHeap::~SharedHeap() noexcept {
        // Liberar TODOS los chunks de cada slab (no solo el primero).
        for (uint32_t cls = 0; cls < SHARED_SIZE_CLASS_COUNT; ++cls) {
            SharedSlab &s     = slabs_[cls];
            uint32_t    count = s.chunks_count.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < count; ++i) {
                if (s.chunks[i].base != nullptr) {
                    size_t chunk_bytes = static_cast<size_t>(s.slot_size) *
                                         static_cast<size_t>(s.slots_per_chunk);
                    vm::free_memory(s.chunks[i].base, chunk_bytes);
                    s.chunks[i].base = nullptr;
                    s.chunks[i].end  = nullptr;
                }
            }
            s.chunks_count.store(0, std::memory_order_release);
        }
    }

    // ----------------------------------------------------------------------
    // Inicializacion del primer chunk del slab
    // ----------------------------------------------------------------------

    bool SharedHeap::init_slab(uint32_t cls) noexcept {
        SharedSlab &s     = slabs_[cls];
        s.slot_size       = SHARED_SIZE_CLASSES[cls];
        s.slots_per_chunk = SHARED_SLOTS_PER_CLASS[cls];

        // Reservar el primer chunk.
        size_t chunk_bytes = static_cast<size_t>(s.slot_size) *
                             static_cast<size_t>(s.slots_per_chunk);
        void *raw = vm::allocate_memory(chunk_bytes,
                                        vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (raw == nullptr) {
            s.chunks_count.store(0, std::memory_order_relaxed);
            s.free_head.store(0, std::memory_order_relaxed);
            return false;
        }
        s.chunks[0].base = static_cast<uint8_t *>(raw);
        s.chunks[0].end  = s.chunks[0].base + chunk_bytes;
        s.chunks_count.store(1, std::memory_order_release);

        // Construir la free list inicial (single-thread, sin CAS).
        SharedFreeNode *first = nullptr;
        SharedFreeNode *last  = nullptr;
        build_chunk_chain(s.chunks[0].base, s.slot_size, s.slots_per_chunk,
                          &first, &last);
        // last->next ya esta en null por build_chunk_chain.

        // Publicar la cabeza con tag 0.
        uintptr_t initial_head = shared_pack_tagged(first, 0);
        s.free_head.store(initial_head, std::memory_order_relaxed);
        s.alloc_count.store(0, std::memory_order_relaxed);
        s.free_count.store(0, std::memory_order_relaxed);
        s.growing.store(0, std::memory_order_relaxed);
        return true;
    }

    // ----------------------------------------------------------------------
    // grow_slab: aloca un chunk adicional y thread sus slots al free list
    // ----------------------------------------------------------------------

    bool SharedHeap::grow_slab(uint32_t cls) noexcept {
        SharedSlab &s = slabs_[cls];

        // Intentar tomar el flag de growth.  Si pierde, spin-waitea.
        uint32_t expected = 0;
        if (!s.growing.compare_exchange_strong(expected, 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Otro hilo esta creciendo.  Spin con PAUSE hasta que termine.
            // Bound conservador: 1M iters (~5 ms en CPU moderno).  Si no
            // termina en ese tiempo, asumimos progreso y reintentamos.
            for (uint32_t i = 0; i < 1000000; ++i) {
                if (s.growing.load(std::memory_order_acquire) == 0) break;
                SHARED_HEAP_PAUSE();
            }
            // Tras la espera, retornamos true: el ganador habra agregado
            // slots (o el max esta alcanzado y el alloc reintentara y dara
            // null limpio).  El caller hace el pop normal.
            return true;
        }

        // Ganamos el flag.  Verificar si despues de tomar el lock el
        // free_head ya tiene slots (otro hilo crecio mientras esperabamos
        // el CAS sobre `growing`, aunque en este path no es posible
        // porque solo hay un growing al tiempo).
        uintptr_t cur_head = s.free_head.load(std::memory_order_acquire);
        if (shared_unpack_ptr(cur_head) != nullptr) {
            // Ya hay slots: no necesitamos crecer.
            s.growing.store(0, std::memory_order_release);
            return true;
        }

        // Comprobar limite de chunks.
        uint32_t cur_chunks = s.chunks_count.load(std::memory_order_acquire);
        if (cur_chunks >= SHARED_MAX_CHUNKS_PER_SLAB) {
            s.growing.store(0, std::memory_order_release);
            return false;
        }

        // Allocar un chunk nuevo.
        size_t chunk_bytes = static_cast<size_t>(s.slot_size) *
                             static_cast<size_t>(s.slots_per_chunk);
        void *raw = vm::allocate_memory(chunk_bytes,
                                        vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (raw == nullptr) {
            s.growing.store(0, std::memory_order_release);
            return false;
        }
        uint8_t *base = static_cast<uint8_t *>(raw);

        // Construir la cadena del chunk nuevo.
        SharedFreeNode *first = nullptr;
        SharedFreeNode *last  = nullptr;
        build_chunk_chain(base, s.slot_size, s.slots_per_chunk, &first, &last);

        // Publicar el rango del chunk ANTES de hacer visible los slots,
        // para que size_class_of() detecte el rango si otro thread libera
        // un puntero de este chunk durante el push.
        s.chunks[cur_chunks].base = base;
        s.chunks[cur_chunks].end  = base + chunk_bytes;
        s.chunks_count.store(cur_chunks + 1, std::memory_order_release);

        // Push masivo a la pila Treiber: last->next = head_viejo, head = first.
        // CAS loop estandar.
        uintptr_t old_head = s.free_head.load(std::memory_order_acquire);
        for (;;) {
            SharedFreeNode *old_top = static_cast<SharedFreeNode *>(
                                          shared_unpack_ptr(old_head));
            std::atomic_store_explicit(
                reinterpret_cast<std::atomic<SharedFreeNode *> *>(&last->next),
                old_top,
                std::memory_order_relaxed);

            uint16_t  new_tag  = static_cast<uint16_t>(
                                     shared_unpack_tag(old_head) + 1);
            uintptr_t new_head = shared_pack_tagged(first, new_tag);

            if (s.free_head.compare_exchange_weak(
                    old_head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
            // Retry con old_head actualizado.
        }

        // Limpiar el flag.
        s.growing.store(0, std::memory_order_release);
        return true;
    }

    // ----------------------------------------------------------------------
    // Lookup de size-class
    // ----------------------------------------------------------------------

    int SharedHeap::find_class(size_t bytes) const noexcept {
        if (SHARED_HEAP_UNLIKELY(bytes > SHARED_SIZE_CLASSES[SHARED_SIZE_CLASS_COUNT - 1])) {
            return -1;
        }
        if (SHARED_HEAP_UNLIKELY(bytes == 0)) {
            return 0;
        }
#if defined(__GNUC__) || defined(__clang__)
        unsigned bits = (bytes <= 1)
                        ? 0
                        : (64u - static_cast<unsigned>(__builtin_clzll(bytes - 1)));
#elif defined(_MSC_VER)
        unsigned long idx;
        unsigned bits;
        if (bytes <= 1) {
            bits = 0;
        } else if (_BitScanReverse64(&idx, bytes - 1)) {
            bits = static_cast<unsigned>(idx) + 1;
        } else {
            bits = 0;
        }
#else
        unsigned bits = 0;
        size_t   v    = bytes - 1;
        while (v > 0) { v >>= 1; ++bits; }
#endif
        int cls = static_cast<int>(bits) - 4;
        if (cls < 0) cls = 0;
        return cls;
    }

    // ----------------------------------------------------------------------
    // alloc: pop con growth on-demand
    // ----------------------------------------------------------------------

    uint8_t *SharedHeap::alloc(size_t bytes) noexcept {
        int cls = find_class(bytes);
        if (SHARED_HEAP_UNLIKELY(cls < 0)) return nullptr; // > 32 KB
        SharedSlab &s = slabs_[static_cast<uint32_t>(cls)];
        if (SHARED_HEAP_UNLIKELY(s.chunks_count.load(std::memory_order_acquire) == 0)) {
            return nullptr; // slab no init (OOM al construir)
        }

        // Hot path con retry sobre growth.
        // En el caso normal (slab no vacio), el primer iter del outer
        // hace el pop CAS y retorna.  Solo crece cuando el head es null.
        for (;;) {
            uintptr_t cur_head = s.free_head.load(std::memory_order_acquire);
            for (;;) {
                SharedFreeNode *top = static_cast<SharedFreeNode *>(
                                          shared_unpack_ptr(cur_head));
                if (SHARED_HEAP_UNLIKELY(top == nullptr)) {
                    // Slab vacio: intentar crecer.
                    if (!grow_slab(static_cast<uint32_t>(cls))) {
                        // Max chunks alcanzado u OOM real: rendirse.
                        return nullptr;
                    }
                    // Tras grow, romper inner loop y re-cargar head.
                    break;
                }
                SharedFreeNode *next = static_cast<SharedFreeNode *>(
                    std::atomic_load_explicit(
                        reinterpret_cast<std::atomic<SharedFreeNode *> *>(&top->next),
                        std::memory_order_relaxed));

                uint16_t  new_tag  = static_cast<uint16_t>(
                                         shared_unpack_tag(cur_head) + 1);
                uintptr_t new_head = shared_pack_tagged(next, new_tag);

                if (SHARED_HEAP_LIKELY(s.free_head.compare_exchange_weak(
                        cur_head, new_head,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))) {
                    s.alloc_count.fetch_add(1, std::memory_order_relaxed);
                    return reinterpret_cast<uint8_t *>(top);
                }
                // CAS fallo: retry inmediato sin re-load.
            }
            // Salimos del inner via break -> reintentar outer (head re-cargado).
        }
    }

    // ----------------------------------------------------------------------
    // free: push atomico al free list
    // ----------------------------------------------------------------------

    void SharedHeap::free(uint8_t *ptr) noexcept {
        if (SHARED_HEAP_UNLIKELY(ptr == nullptr)) return;

        int cls = size_class_of(ptr);
        if (SHARED_HEAP_UNLIKELY(cls < 0)) return;
        SharedSlab     &s    = slabs_[static_cast<uint32_t>(cls)];
        SharedFreeNode *node = reinterpret_cast<SharedFreeNode *>(ptr);

        uintptr_t cur_head = s.free_head.load(std::memory_order_relaxed);
        for (;;) {
            SharedFreeNode *top = static_cast<SharedFreeNode *>(
                                      shared_unpack_ptr(cur_head));
            std::atomic_store_explicit(
                reinterpret_cast<std::atomic<SharedFreeNode *> *>(&node->next),
                top,
                std::memory_order_relaxed);

            uint16_t  new_tag  = static_cast<uint16_t>(
                                     shared_unpack_tag(cur_head) + 1);
            uintptr_t new_head = shared_pack_tagged(node, new_tag);

            if (SHARED_HEAP_LIKELY(s.free_head.compare_exchange_weak(
                    cur_head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))) {
                s.free_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // ----------------------------------------------------------------------
    // Lookups auxiliares
    // ----------------------------------------------------------------------

    bool SharedHeap::contains(const uint8_t *ptr) const noexcept {
        // Recorrer cada slab y cada chunk.  Hot path: la mayoria de ptrs
        // estaran en el chunk[0] del slab adecuado, asi que en practica
        // hace 12 + N comparaciones donde N es el num medio de chunks.
        for (uint32_t cls = 0; cls < SHARED_SIZE_CLASS_COUNT; ++cls) {
            const SharedSlab &s    = slabs_[cls];
            uint32_t          n    = s.chunks_count.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < n; ++i) {
                const SharedChunkRange &r = s.chunks[i];
                if (r.base != nullptr && ptr >= r.base && ptr < r.end) {
                    return true;
                }
            }
        }
        return false;
    }

    int SharedHeap::size_class_of(const uint8_t *ptr) const noexcept {
        for (uint32_t cls = 0; cls < SHARED_SIZE_CLASS_COUNT; ++cls) {
            const SharedSlab &s = slabs_[cls];
            uint32_t          n = s.chunks_count.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < n; ++i) {
                const SharedChunkRange &r = s.chunks[i];
                if (r.base != nullptr && ptr >= r.base && ptr < r.end) {
                    return static_cast<int>(cls);
                }
            }
        }
        return -1;
    }

    // ----------------------------------------------------------------------
    // Stats (relaxed atomic loads)
    // ----------------------------------------------------------------------

    uint64_t SharedHeap::total_allocated_bytes() const noexcept {
        uint64_t total = 0;
        for (uint32_t cls = 0; cls < SHARED_SIZE_CLASS_COUNT; ++cls) {
            const SharedSlab &s = slabs_[cls];
            uint64_t allocs = s.alloc_count.load(std::memory_order_relaxed);
            uint64_t frees  = s.free_count.load(std::memory_order_relaxed);
            uint64_t live   = (allocs > frees) ? (allocs - frees) : 0;
            total += live * static_cast<uint64_t>(s.slot_size);
        }
        return total;
    }

    uint64_t SharedHeap::alloc_count(uint32_t cls) const noexcept {
        if (cls >= SHARED_SIZE_CLASS_COUNT) return 0;
        return slabs_[cls].alloc_count.load(std::memory_order_relaxed);
    }

    uint64_t SharedHeap::free_count(uint32_t cls) const noexcept {
        if (cls >= SHARED_SIZE_CLASS_COUNT) return 0;
        return slabs_[cls].free_count.load(std::memory_order_relaxed);
    }

} // namespace gc
