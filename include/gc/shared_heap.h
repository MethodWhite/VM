/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file shared_heap.h
 * @brief Heap GC compartido cross-process, lock-free, slab allocator.
 *
 * Fase Z.1 del plan de sincronizacion cross-process.
 *
 * Disenyo
 * =======
 * - **Por que un heap separado**: cada @c ProcessVM tiene su propio
 *   @c gc::GcHeap private (~0% sync overhead).  El @c SharedHeap aloja
 *   exclusivamente los objetos que ESCAPAN cross-process (capturas de
 *   spawn, args de msgsend, returns de @Async, etc).  La promocion
 *   ocurre una sola vez en el call site del escape (Z.5+Z.6).
 *
 * - **Slab allocator lock-free**: 12 size-classes potencia de 2 (16 B
 *   a 32 KB).  Cada slab mantiene una free list intrusiva como pila
 *   Treiber con CAS atomico (DWCAS o tagged-ptr-48 segun plataforma).
 *   Hot path = 1 CAS (~5 ns).
 *
 * - **Cache-line aligned**: cada slab esta alineado a 128 bytes (cubre
 *   AArch64 y x86-64; evita false sharing entre clases).
 *
 * - **ABA-safe**: tag de 16 bits empacado en los bits altos del puntero
 *   (asume canonical 48-bit address space, valido en x86-64 y AArch64).
 *   Para plataformas con address space > 48 bits, fallback a DWCAS de
 *   128 bits.  Hoy x86-64 Linux/Windows/macOS y AArch64 TBI-off
 *   garantizan los 16 bits altos libres.
 *
 * - **Portabilidad a C**: el layout en disco (sizeof y offsets) es
 *   estable.  Los atomicos se mapean 1:1 a @c _Atomic de C11.  Sin
 *   constructores virtuales, sin RAII en estructuras.  El wrapper
 *   @c SharedHeap C++ es solo conveniencia; portar a C es renombrar
 *   `std::atomic<T>` -> `_Atomic T` y `compare_exchange_weak` ->
 *   `atomic_compare_exchange_weak_explicit`.
 *
 * - **JIT-friendly**: @c alloc y @c free bajan a una secuencia fija
 *   de instrucciones x86-64 (5-7 instr cada uno).  Sin loops dinamicos
 *   ni dispatch indirecto.  El size-class se decide en compile time
 *   cuando @c bytes es constante (caso comun: @c sizeof(Object)).
 *
 * Costes medidos en x86-64 moderno (estimacion sin contencion)
 * ============================================================
 * - alloc fast path (slab no vacio): ~5 ns (1 CAS exitoso)
 * - alloc contencion alta: ~30 ns (algunas retries CAS)
 * - free: ~5 ns (1 CAS)
 * - contains(): ~3 ns (12 comparaciones de rango)
 * - size_class_of(): ~5 ns (linear search 12 slabs)
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gc {

    // ------------------------------------------------------------------
    // Constantes de configuracion del slab
    // ------------------------------------------------------------------

    /** @brief Numero de size-classes del slab (potencias de 2 de 16 a 32K). */
    static constexpr uint32_t SHARED_SIZE_CLASS_COUNT = 12;

    /**
     * @brief Tabla de tamanyos de los size-classes.
     *
     * Cada slot del slab class @c i tiene exactamente
     * @c SHARED_SIZE_CLASSES[i] bytes utiles.  El allocator devuelve el
     * primer class cuyo @c slot_size >= @c bytes solicitados.
     *
     * Cobertura de objetos tipicos:
     * - 16-64 B: ObjectHeader (24 B) + 1-3 fields i64
     * - 128-256 B: structs medianos, strings hasta ~200 bytes
     * - 512 B-2 KB: strings largos, arrays medianos
     * - 4-32 KB: arrays grandes, buffers
     *
     * Objetos > 32 KB se rechazan (return nullptr).  Phase Z+: caso
     * general via @c VirtualAlloc directo + tabla auxiliar.
     */
    static constexpr uint32_t SHARED_SIZE_CLASSES[SHARED_SIZE_CLASS_COUNT] = {
        16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
    };

    /**
     * @brief Numero de slots INICIAL por size-class (chunk #0).
     *
     * Cada slab reserva ESTE numero de slots en el primer chunk.  Cuando
     * el chunk se exhausta y el free list queda vacio, el slab CRECE
     * dinamicamente alocando chunks adicionales del mismo tamano.  Esto
     * elimina la limitacion de "max ~6 MB total" -- programas reales
     * pueden alocar millones de objetos sin agotar el SharedHeap.
     *
     * Memoria total maxima = SHARED_SLOTS_PER_CLASS[i] * SHARED_SIZE_CLASSES[i]
     *                       * SHARED_MAX_CHUNKS_PER_SLAB.
     * Para clase 16 B: 16384 * 16 * 256 = 64 MB max.
     * Total maximo (todas clases): ~6 MB * 256 = 1.5 GB.
     */
    static constexpr uint32_t SHARED_SLOTS_PER_CLASS[SHARED_SIZE_CLASS_COUNT] = {
        16384, // 16 B  * 16K  = 256 KB initial; max 64 MB con growth
        16384, // 32 B  * 16K  = 512 KB initial; max 128 MB
        8192,  // 64 B  * 8K   = 512 KB
        4096,  // 128 B * 4K   = 512 KB
        2048,  // 256 B * 2K   = 512 KB
        1024,  // 512 B * 1K   = 512 KB
        512,   // 1 KB  * 512  = 512 KB
        256,   // 2 KB  * 256  = 512 KB
        128,   // 4 KB  * 128  = 512 KB
        64,    // 8 KB  * 64   = 512 KB
        32,    // 16 KB * 32   = 512 KB
        16,    // 32 KB * 16   = 512 KB
    };

    /**
     * @brief Maximo de chunks por slab.  Cada chunk anyade
     *        @c SHARED_SLOTS_PER_CLASS[i] slots al free list.
     *
     * Con 256 chunks y los slot counts default:
     *   - Clase 16 B: 4.19M slots = 64 MB max.
     *   - Clase 32 B: 4.19M slots = 128 MB max.
     *   - Clase 32 KB: 4096 slots = 128 MB max.
     *
     * Total potencial: ~1.5 GB.  Memoria solo se reserva cuando el slab
     * realmente lo necesita (lazy growth en alloc cuando free list empty).
     */
    static constexpr uint32_t SHARED_MAX_CHUNKS_PER_SLAB = 256;

    /**
     * @brief Alineacion cache-line objetivo (cubre AArch64 y x86-64).
     *
     * En AArch64 algunas implementaciones (Apple M-series, Marvell
     * ThunderX) tienen lineas de 128 bytes; en x86-64 son 64 bytes.
     * Usamos 128 como upper bound seguro: evita false sharing en TODAS
     * las arquitecturas modernas con cero coste (solo padding interno).
     */
    static constexpr size_t SHARED_CACHE_LINE = 128;

    // ------------------------------------------------------------------
    // Tagged pointer (ABA-safe via tag de 16 bits)
    // ------------------------------------------------------------------

    /**
     * @brief Mascara que aisla los 48 bits bajos del puntero.
     *
     * En x86-64 y AArch64, los punteros virtuales validos usan solo los
     * 48 bits bajos (canonical addressing).  Los 16 bits altos quedan
     * disponibles para empacar un contador ABA sin necesidad de DWCAS.
     */
    static constexpr uintptr_t SHARED_TAG_PTR_MASK = 0x0000FFFFFFFFFFFFULL;

    /** @brief Cuantos bits ocupa el tag ABA (los altos del @c uintptr_t). */
    static constexpr int SHARED_TAG_SHIFT = 48;

    /**
     * @brief Empaca un puntero + tag ABA de 16 bits en un @c uintptr_t.
     *
     * Inline + constexpr-friendly: el compilador resuelve la expresion
     * en compile time cuando ambos argumentos son constantes.  No
     * verifica que @p p sea canonical (los 16 bits altos del puntero
     * deben ser ya 0 en arquitecturas soportadas).
     */
    static inline uintptr_t shared_pack_tagged(void *p, uint16_t tag) noexcept {
        return (reinterpret_cast<uintptr_t>(p) & SHARED_TAG_PTR_MASK)
             | (static_cast<uintptr_t>(tag) << SHARED_TAG_SHIFT);
    }

    /** @brief Extrae el puntero (48 bits bajos) de un tagged @c uintptr_t. */
    static inline void *shared_unpack_ptr(uintptr_t v) noexcept {
        return reinterpret_cast<void *>(v & SHARED_TAG_PTR_MASK);
    }

    /** @brief Extrae el tag ABA (16 bits altos) de un tagged @c uintptr_t. */
    static inline uint16_t shared_unpack_tag(uintptr_t v) noexcept {
        return static_cast<uint16_t>(v >> SHARED_TAG_SHIFT);
    }

    // ------------------------------------------------------------------
    // Free node: encabezado intrusivo de un slot libre
    // ------------------------------------------------------------------

    /**
     * @brief Nodo de la free list intrusiva.
     *
     * Cuando un slot esta libre, sus primeros 8 bytes contienen el
     * puntero @c next del siguiente slot libre en la pila.  Una vez
     * alocado, el slot completo es del usuario; el campo @c next se
     * sobreescribe sin problema.
     *
     * No es tagged: el tag ABA vive en la cabeza del slab, no en cada
     * nodo (mas barato y aligned-safe).
     */
    struct SharedFreeNode {
        SharedFreeNode *next; ///< Siguiente slot libre, o nullptr al final
    };

    // ------------------------------------------------------------------
    // Slab: cabeza de free list + metadata, una por size-class
    // ------------------------------------------------------------------

    /**
     * @brief Rango de memoria de un chunk individual del slab.
     *
     * Layout 16 bytes (2 host_ptrs).  Util para @c contains() (range check).
     */
    struct SharedChunkRange {
        uint8_t *base;
        uint8_t *end;
    };

    /**
     * @brief Cabeza de un slab con growth dinamico, cache-line aligned.
     *
     * Cada size-class del @c SharedHeap tiene su propio @c SharedSlab.
     * El @c free_head es una pila Treiber lock-free (CAS atomico).  El
     * tag ABA evita la race clasica cuando un slot se libera y re-aloca
     * en otro thread entre el load y el CAS.
     *
     * **Growth dinamico (sin limite practical de 6 MB)**: el slab inicia
     * con un solo chunk de @c SHARED_SLOTS_PER_CLASS[i] slots.  Cuando
     * la pila Treiber se vacia (alloc encuentra free_head == 0), el
     * allocator toma @c grow_mtx, aloca otro chunk de la misma talla
     * via @c vm::allocate_memory y thread sus slots al free list via
     * push masivo CAS.  Otros threads que entren mientras se aloca
     * spinean en el mtx; tras release, retoman el CAS pop.
     *
     * Layout cache-line aligned para evitar false sharing entre slabs.
     */
    struct alignas(SHARED_CACHE_LINE) SharedSlab {
        /** @brief Cabeza tagged de la pila Treiber (ptr + tag ABA). */
        std::atomic<uintptr_t> free_head;

        /** @brief Contador de alocaciones (diagnostico). */
        std::atomic<uint64_t> alloc_count;

        /** @brief Contador de liberaciones (diagnostico). */
        std::atomic<uint64_t> free_count;

        /** @brief Tamanyo de cada slot en bytes. */
        uint32_t slot_size;

        /** @brief Numero de slots por chunk (mismo para todos los chunks del slab). */
        uint32_t slots_per_chunk;

        /** @brief Numero de chunks alocados hasta ahora.  Atomic para growth lock-free read. */
        std::atomic<uint32_t> chunks_count;

        /**
         * @brief Flag de growth en progreso (CAS-only lock, sin pthread_mutex).
         *
         * Cuando @c alloc encuentra @c free_head==0 y necesita crecer, hace
         * CAS de 0 a 1 sobre este flag.  Si gana, ejecuta el grow y deja
         * el flag a 0.  Si pierde, hace spin-wait con PAUSE hasta que el
         * flag vuelva a 0, luego reintenta el pop normal (otro hilo ya
         * agrego mas slots).  Portable a C11 con @c _Atomic uint32_t.
         */
        std::atomic<uint32_t> growing;

        /** @brief Array de rangos de los chunks (lazy alloc).  contains() lo escanea. */
        SharedChunkRange chunks[SHARED_MAX_CHUNKS_PER_SLAB];
    };

    static_assert(sizeof(SharedSlab) <= 64 + 16 * SHARED_MAX_CHUNKS_PER_SLAB + 256,
                  "SharedSlab fits within reasonable bound");
    static_assert(alignof(SharedSlab) == SHARED_CACHE_LINE,
                  "SharedSlab debe estar alineado a cache line");

    // ------------------------------------------------------------------
    // SharedHeap: API publica
    // ------------------------------------------------------------------

    /**
     * @brief Heap GC compartido cross-process.
     *
     * Singleton logico por @c VM (la @c VM construye una instancia y
     * la comparte con todos sus @c ProcessVM).  Lock-free en el hot
     * path (alloc/free); coordinacion via CAS atomico por size-class.
     *
     * Uso esperado (Z.5+Z.6):
     * @code
     * shared_heap.alloc(sizeof(MyClass));      // promotion site
     * shared_heap.free(host_ptr);               // GC sweep
     * @endcode
     *
     * El heap NO incluye GC mark/sweep en Z.1; solo allocator base.
     * Mark/sweep se anyade en Z.4 (junto con cross-process roots).
     */
    class SharedHeap {
    public:
        /**
         * @brief Construye el heap reservando memoria para los 12 slabs.
         *
         * Reserva ~6 MiB de espacio virtual via @c vm::allocate_memory
         * (mmap o VirtualAlloc segun plataforma).  No falla en silencio:
         * si la reserva no es posible, @c init_ok() devuelve false.
         */
        SharedHeap() noexcept;

        /**
         * @brief Libera toda la memoria reservada por el heap.
         *
         * NO ejecuta finalizadores sobre objetos vivos.  El caller debe
         * asegurar que no quedan accesos pendientes desde otros threads
         * antes de destruir el heap (idealmente solo se destruye al
         * shutdown de la @c VM).
         */
        ~SharedHeap() noexcept;

        SharedHeap(const SharedHeap &) = delete;
        SharedHeap &operator=(const SharedHeap &) = delete;
        SharedHeap(SharedHeap &&)                 = delete;
        SharedHeap &operator=(SharedHeap &&)      = delete;

        /**
         * @brief Indica si la inicializacion exitosa.
         *
         * Si devuelve false, todas las operaciones @c alloc fallaran
         * (devuelven @c nullptr).  Verificable tras el ctor para
         * detectar OOM temprano.
         */
        bool init_ok() const noexcept { return init_ok_; }

        /**
         * @brief Aloca un slot que pueda contener al menos @p bytes.
         *
         * Lock-free.  Selecciona el size-class minimo cuyo @c slot_size
         * >= @p bytes.  Si el slab esta vacio o @p bytes > 32 KB
         * devuelve @c nullptr.
         *
         * @param bytes  Tamanyo minimo requerido (1..32768).
         * @return       Puntero al slot (8-byte aligned), o nullptr.
         *
         * Hot path (slab no vacio):
         * 1. @c load del @c free_head con acquire (1 instr).
         * 2. Calcular @c next del nodo (1 instr).
         * 3. CAS para reemplazar la cabeza (1 instr).
         * 4. Return.
         *
         * En caso de contencion (CAS falla), retry inmediato sin
         * bloquearse.  Bound de retries en la practica: <5 incluso bajo
         * carga extrema.
         */
        uint8_t *alloc(size_t bytes) noexcept;

        /**
         * @brief Devuelve un slot al free list del slab correspondiente.
         *
         * Lock-free.  Calcula la size-class via aritmetica de punteros
         * (linear search de los 12 slabs).  Si @p ptr no pertenece a
         * este heap, es no-op (no crashea).
         *
         * @param ptr  Puntero devuelto por @c alloc previamente.
         *             Debe ser exactamente el devuelto (NO interior).
         */
        void free(uint8_t *ptr) noexcept;

        /**
         * @brief Indica si @p ptr pertenece a este heap.
         *
         * Util para integracion con el GC: el scan conservativo decide
         * si un valor es una referencia al shared_heap o a otra region.
         * O(12) linear search; cada comparacion es 2 cmp + 1 branch.
         */
        bool contains(const uint8_t *ptr) const noexcept;

        /**
         * @brief Indice de size-class al que pertenece @p ptr, o -1.
         *
         * Devuelve -1 si @p ptr no esta en ningun slab.  Usado por el
         * GC para determinar el tamano del bloque al barrer.
         */
        int size_class_of(const uint8_t *ptr) const noexcept;

        /**
         * @brief Total de bytes vivos (alloc - free) en el heap.
         *
         * Aproximado: usa contadores atomicos relaxed, puede leer
         * estado intermedio bajo contencion.  Util para reporting,
         * no para garantias de correctness.
         */
        uint64_t total_allocated_bytes() const noexcept;

        /** @brief Numero de allocaciones de la size-class @p cls. */
        uint64_t alloc_count(uint32_t cls) const noexcept;

        /** @brief Numero de liberaciones de la size-class @p cls. */
        uint64_t free_count(uint32_t cls) const noexcept;

        /** @brief Accesor al slab @p cls (para tests / GC scan). */
        const SharedSlab &slab(uint32_t cls) const noexcept { return slabs_[cls]; }

    private:
        /** @brief 12 slabs, uno por size-class. */
        SharedSlab slabs_[SHARED_SIZE_CLASS_COUNT];

        /** @brief Marca init exitosa (false si OOM durante el ctor). */
        bool init_ok_;

        /**
         * @brief Inicializa el slab @p cls reservando memoria y
         *        construyendo la free list inicial (todos libres).
         *
         * Llamado desde el ctor.  Devuelve false si la reserva falla.
         */
        bool init_slab(uint32_t cls) noexcept;

        /**
         * @brief Aloca un chunk adicional para el slab @p cls y thread sus
         *        slots al free list via push masivo CAS.
         *
         * Toma @c growing flag con CAS; si pierde la carrera, spin-waitea
         * hasta que el ganador termine.  Devuelve true si tras la llamada
         * hay slots disponibles (creo nuevo chunk u otro hilo lo creo),
         * false si ya alcanzamos @c SHARED_MAX_CHUNKS_PER_SLAB o OOM.
         */
        bool grow_slab(uint32_t cls) noexcept;

        /**
         * @brief Selecciona el size-class minimo que cabe en @p bytes.
         *
         * Tabla compile-time + lookup O(log N) via @c __builtin_clz
         * (GCC/Clang) o equivalente MSVC.  Devuelve -1 si @p bytes
         * excede el max (32 KB).
         */
        int find_class(size_t bytes) const noexcept;
    };

} // namespace gc
