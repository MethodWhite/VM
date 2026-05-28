/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file shared_handle_table.h
 * @brief Tabla global lock-free de handles para objetos del SharedHeap.
 *
 * Diseno (Phase Z.5)
 * ==================
 *
 * - **Indice global por VM**: una sola tabla compartida por todos los
 *   procesos de la misma @c runtime::VM.  Cada slot mantiene un
 *   @c host_ptr atomico al payload del objeto en el SharedHeap.
 *
 * - **Handle bit-31 = SHARED**: el @c GcHandle global (uint32_t) usa
 *   el bit 31 (msb) como tag: bit=1 -> handle compartido, bit=0 ->
 *   handle local (per-process gc_heap, retrocompatible).  Esto da
 *   ~2 mil millones de handles compartidos disponibles (mas que
 *   suficiente para programas realistas).
 *
 * - **Lock-free**: alloc de slot via CAS sobre @c next_free_ con
 *   free list intrusiva (los slots libres apuntan al siguiente).
 *   Lookup es un atomic load sin barrera.  No mutex globales.
 *
 * - **Cache-friendly**: la tabla es un array contiguo de
 *   @c std::atomic<HandleSlot>; los slots tienen 16 bytes (puntero
 *   + metadata empaquetada) -> 4 slots por cache line.
 *
 * - **Tamano fijo + crecimiento por chunks**: para evitar el coste
 *   de un resize lock-free completo, la tabla se compone de chunks
 *   de 16384 slots cada uno, organizados como array de punteros
 *   atomic a chunks.  El primer chunk se asigna en el constructor;
 *   los siguientes se asignan lazy via CAS cuando se quedan sin
 *   slots libres.
 *
 * - **Portabilidad**: @c std::atomic<uint64_t> + CAS son C11 1:1.
 *   El layout binario es ABI-estable (no incluye padding raro).
 *
 * - **JIT-friendly**: lookup baja a `mov eax, [chunks+chunk_idx*8];
 *   mov rax, [eax+slot_offset]` (~5 ciclos cuando esta caliente).
 *
 * Coste estimado
 * ==============
 * - register_object: 1 CAS (lock-free) + 1 store host_ptr = ~10 ns
 * - lookup: 2 loads (chunk + slot) = ~5 ns
 * - unregister: 1 CAS para poner el slot en free list = ~5 ns
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gc {

    /**
     * @brief Bit indicador en GcHandle: 1 = handle global compartido.
     *
     * Cuando @c GcHandle tiene bit 31 set, se interpreta como
     * indice en la @c SharedHandleTable.  El indice efectivo es
     * @c (handle & SHARED_HANDLE_MASK).
     */
    static constexpr uint32_t SHARED_HANDLE_BIT  = 0x80000000u;

    /** @brief Mascara para extraer el indice de un shared handle. */
    static constexpr uint32_t SHARED_HANDLE_MASK = 0x7FFFFFFFu;

    /** @brief Sentinel "no shared handle" (equivalente a GC_NULL_HANDLE local). */
    static constexpr uint32_t SHARED_NULL_HANDLE = 0;

    /**
     * @brief Slot de la tabla.  Layout 16 bytes para alineacion a 16.
     *
     * El @c host_ptr es atomic para permitir CAS en register/unregister
     * sin race.  El @c next_free conecta slots libres en una lista
     * intrusiva (Treiber stack).
     *
     * Estado del slot:
     *   host_ptr == 0     ->  slot libre; next_free apunta al siguiente
     *                          libre (o 0 al final).
     *   host_ptr != 0     ->  slot ocupado; next_free es 0.
     */
    struct alignas(16) HandleSlot {
        std::atomic<uint64_t> host_ptr;  ///< Puntero al payload (0 si libre)
        std::atomic<uint32_t> next_free; ///< Indice del siguiente slot libre
        uint32_t               size_bytes; ///< Tamano del slot (para GC scan)
    };
    static_assert(sizeof(HandleSlot) == 16,
                  "HandleSlot debe ocupar exactamente 16 bytes");
    static_assert(alignof(HandleSlot) == 16,
                  "HandleSlot debe alinearse a 16");

    /**
     * @brief Tabla global lock-free de handles compartidos.
     *
     * Singleton logico por @c runtime::VM.  Los slots se organizan en
     * chunks de 16K elementos cada uno; los chunks se alocan lazy via
     * @c VirtualAlloc / @c mmap (alineados a pagina).
     */
    class SharedHandleTable {
    public:
        /** @brief Slots por chunk (16K x 16 B = 256 KB por chunk). */
        static constexpr uint32_t CHUNK_SLOTS = 16384;

        /** @brief Maximo de chunks (= 32K handles totales soportados). */
        static constexpr uint32_t MAX_CHUNKS = 256;

        /**
         * @brief Maximo total de handles compartidos.
         *
         * 16384 slots/chunk * 256 chunks = 4.19M handles.  El bit 31
         * del @c GcHandle uint32 permite 2^31 = 2.14G handles, asi que
         * estamos lejos del limite del tipo.
         */
        static constexpr uint32_t MAX_HANDLES = CHUNK_SLOTS * MAX_CHUNKS;

        SharedHandleTable() noexcept;
        ~SharedHandleTable() noexcept;

        SharedHandleTable(const SharedHandleTable &) = delete;
        SharedHandleTable &operator=(const SharedHandleTable &) = delete;
        SharedHandleTable(SharedHandleTable &&)                 = delete;
        SharedHandleTable &operator=(SharedHandleTable &&)      = delete;

        /**
         * @brief Indica si la tabla esta lista para usar.
         *
         * False si el primer chunk no pudo alocarse al construir.
         */
        bool init_ok() const noexcept { return init_ok_; }

        /**
         * @brief Registra @p host_ptr como objeto compartido y devuelve
         *        su GcHandle global (con bit 31 set).
         *
         * Lock-free.  Pop atomico del free list.  Si el free list esta
         * vacio, intenta crecer la tabla (alocar otro chunk).
         *
         * @param host_ptr   Puntero al payload del objeto (en SharedHeap).
         * @param size_bytes Tamano del objeto (para GC scan futuro).
         * @return GcHandle con bit 31 set, o 0 (SHARED_NULL_HANDLE) si OOM.
         */
        uint32_t register_object(uint8_t *host_ptr, uint32_t size_bytes) noexcept;

        /**
         * @brief Resuelve un shared handle a su @c host_ptr actual.
         *
         * Lock-free (un solo atomic load).  Acepta @p handle con o sin
         * bit 31 (lo ignora internamente).  Si el handle es invalido o
         * libre, devuelve @c nullptr.
         *
         * @param handle Handle compartido (con o sin bit 31).
         * @return Puntero al payload, o nullptr si invalido/libre.
         */
        uint8_t *lookup(uint32_t handle) const noexcept;

        /**
         * @brief Libera un slot devolviendolo al free list.
         *
         * Lock-free (CAS).  Tras la llamada, el slot puede ser reusado
         * por @c register_object.  El caller es responsable de garantizar
         * que ningun otro thread esta accediendo al objeto en este
         * instante (semantica typica del GC sweep).
         */
        void unregister(uint32_t handle) noexcept;

        /** @brief Numero de slots actualmente ocupados (snapshot relaxed). */
        uint32_t live_count() const noexcept;

        /** @brief Numero de chunks alocados. */
        uint32_t chunk_count() const noexcept;

        // ----------------------------------------------------------------
        // Phase Z.10 ext: mark/sweep STW del SharedHeap.
        //
        // El bitmap @c mark_bits_ es paralelo al array de slots; tiene 1
        // bit por slot.  Cero overhead durante operacion normal (solo se
        // usa durante GC sweep).  Memoria: 1 bit por slot = 4096 slots
        // por chunk = 512 bytes/chunk de bitmap.
        //
        // Protocolo:
        //   1. clear_marks(): zero el bitmap entero (start of GC).
        //   2. mark(handle): set bit del slot correspondiente.
        //   3. is_marked(handle): test bit.
        //   4. sweep_unmarked(callback): para cada slot vivo NO marcado,
        //      llama @p callback(host_ptr, size_bytes) y unregister.
        // ----------------------------------------------------------------

        /** @brief Pone a 0 todos los bits del mark bitmap.  STW assumed. */
        void clear_marks() noexcept;

        /** @brief Marca el slot del handle como reachable.  No atomic (STW). */
        void mark(uint32_t handle) noexcept;

        /** @brief Comprueba si el slot esta marcado. */
        bool is_marked(uint32_t handle) const noexcept;

        // sweep_unmarked y for_each_live son templates inline definidos abajo.

    private:
        /** @brief Array de punteros a chunks; cada chunk = 16K slots. */
        std::atomic<HandleSlot *> chunks_[MAX_CHUNKS];

        /**
         * @brief Bitmap de marca paralelo a @c chunks_ para GC mark/sweep.
         *
         * Cada chunk de 16384 slots usa 16384 bits = 2048 bytes = 256 u64s.
         * Lazy alloc: solo se crea cuando se hace el primer @c clear_marks
         * para ese chunk.  Cero overhead si no se usa GC del SharedHeap.
         */
        std::atomic<uint64_t *> mark_chunks_[MAX_CHUNKS];

        /** @brief U64 words por chunk de mark bits (16384 bits / 64). */
        static constexpr uint32_t MARK_WORDS_PER_CHUNK = CHUNK_SLOTS / 64;

    public:
        // ----------------------------------------------------------------
        // Implementaciones inline de los templates de iteracion.
        // Deben estar en el header para que se instancien en cada TU que
        // los usa (lambda callback distinto en cada call site).
        // ----------------------------------------------------------------

        template <typename Cb>
        uint32_t sweep_unmarked(Cb cb) noexcept {
            uint32_t swept = 0;
            uint32_t total = next_fresh_handle_.load(std::memory_order_relaxed);
            // Indice 0 reservado (SHARED_NULL_HANDLE); empezar desde 1.
            for (uint32_t idx = 1; idx < total; ++idx) {
                HandleSlot *s = slot_at(idx);
                if (!s) continue;
                uint64_t p = s->host_ptr.load(std::memory_order_relaxed);
                if (p == 0) continue; // slot libre
                if (is_marked(idx | SHARED_HANDLE_BIT)) continue; // alcanzable
                // No marcado y vivo: barrer.
                uint32_t sz = s->size_bytes;
                cb(reinterpret_cast<uint8_t *>(p), sz);
                // Unregister directo: clear ptr + push al free list.
                unregister(idx | SHARED_HANDLE_BIT);
                ++swept;
            }
            return swept;
        }

        template <typename Cb>
        void for_each_live(Cb cb) const noexcept {
            uint32_t total = next_fresh_handle_.load(std::memory_order_relaxed);
            for (uint32_t idx = 1; idx < total; ++idx) {
                const HandleSlot *s = const_cast<SharedHandleTable *>(this)->slot_at(idx);
                if (!s) continue;
                uint64_t p = s->host_ptr.load(std::memory_order_relaxed);
                if (p == 0) continue;
                cb(idx | SHARED_HANDLE_BIT,
                    reinterpret_cast<uint8_t *>(p),
                    s->size_bytes);
            }
        }

        /** @brief Cabeza del free list intrusivo (tagged ABA-safe). */
        std::atomic<uint64_t> free_list_head_;

        /** @brief Contador atomic del proximo handle a alocar (allocs incrementales). */
        std::atomic<uint32_t> next_fresh_handle_;

        /** @brief Numero de chunks alocados (1 mas que el ultimo idx valido). */
        std::atomic<uint32_t> num_chunks_;

        /** @brief Slots ocupados (incrementado en register, decrementado en unregister). */
        std::atomic<uint32_t> live_count_;

        /** @brief Init exitosa (primer chunk alocado). */
        bool init_ok_;

        /**
         * @brief Aloca un chunk nuevo via VirtualAlloc/mmap.
         *
         * @param chunk_idx Indice del chunk en @c chunks_ a inicializar.
         * @return true si exitoso.
         */
        bool alloc_chunk(uint32_t chunk_idx) noexcept;

        /**
         * @brief Devuelve puntero al slot con indice global @p idx.
         *
         * Resuelve chunk_idx + slot_idx_in_chunk.  Devuelve nullptr si
         * el chunk no esta alocado.
         */
        inline HandleSlot *slot_at(uint32_t idx) const noexcept {
            uint32_t chunk_idx = idx / CHUNK_SLOTS;
            uint32_t slot_idx  = idx % CHUNK_SLOTS;
            if (chunk_idx >= MAX_CHUNKS) return nullptr;
            HandleSlot *chunk = chunks_[chunk_idx].load(std::memory_order_acquire);
            if (!chunk) return nullptr;
            return &chunk[slot_idx];
        }
    };

} // namespace gc
