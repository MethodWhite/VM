/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file wait_table.h
 * @brief Tabla de wait queues lock-free per-bucket (Phase Z.4).
 *
 * Diseno
 * ======
 * - Reemplaza los antiguos @c std::unordered_map<GcHandle, std::vector<...>>
 *   que vivian en GcHeap (no thread-safe, requerian mutex global implicito).
 *
 * - **Spinlock per-bucket**: cada bucket protege su propia chain con un
 *   @c std::atomic_flag.  La granularidad de contencion es 1/BUCKETS
 *   (= 1/4096); en uso normal, dos waiters distintos compiten solo si
 *   sus handles colisionan en hash.  Spin holdtime tipico: <100 ns.
 *
 * - **Sin mutex global**: ni siquiera para resize.  El tamanyo es fijo
 *   (4096 buckets) y se asigna al construir.  Si una entry colisiona
 *   demasiado (raro), se chain-lista dentro del bucket.
 *
 * - **Split por kind**: las queues @c MONITOR (waiters de monenter
 *   bloqueados) y @c CONDVAR (waiters de monwait) son INDEPENDIENTES
 *   incluso para el mismo handle.  Esto fix el bug t13 donde un
 *   @c notify despertaba un @c monenter en lugar de un @c monwait.
 *
 * - **Cache-line aligned**: cada @c Bucket ocupa exactamente 64 bytes
 *   (1 cache line en x86-64; 128 en AArch64 con padding manual) para
 *   evitar false sharing entre buckets adyacentes.
 *
 * Coste (estimacion x86-64)
 * =========================
 * - push: ~10 ns sin contencion (1 spinlock acquire/release + 1 vector::push_back)
 * - pop_one: ~12 ns sin contencion (idem + vector::erase del frente)
 * - pop_all: ~10 ns + O(N) move (N tipico < 10)
 *
 * Portabilidad
 * ============
 * - C: traducir @c std::atomic_flag a @c atomic_flag (C11) y la chain
 *   de @c Entry a una lista enlazada manual.  Mismo ABI esencial.
 *
 * - Sin allocacion en push: las @c Entry se crean lazy y reusan; el
 *   queue interno (@c std::vector) podria reemplazarse en v2 por un
 *   ring buffer fijo de N=16 con overflow a heap si crece (zero-alloc
 *   en el caso comun).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gc {

    /**
     * @brief Tipo de cola de waiters dentro de @c WaitTable.
     *
     * Mantenemos colas SEPARADAS por kind para el mismo handle.  Esto
     * fix el bug t13 (Z.0) donde @c notify popeaba un waiter de
     * @c monenter cuando debia popear uno de @c monwait.
     */
    enum class WaitKind : uint8_t {
        MONITOR = 0, ///< Bloqueado en monenter (esperando adquirir el mutex)
        CONDVAR = 1, ///< Suspendido en monwait (esperando notify/notifyAll)
    };

    /**
     * @brief Tabla hash lock-free per-bucket de wait queues.
     *
     * Indexada por @c (handle, kind), almacena un FIFO de @c encoded_pid
     * (@c (scheduler_id << 32) | local_pid).  Operaciones push/pop son
     * thread-safe via spinlock por bucket.
     */
    class WaitTable {
    public:
        /**
         * @brief Numero de buckets de la tabla.
         *
         * Potencia de 2 (4096): mask binaria barata para el hash.
         * Total memoria: 4096 * 64 B = 256 KB.  Se asigna al construir;
         * sin redimensionamiento dinamico (capacidad fija conocida).
         */
        static constexpr uint32_t BUCKETS = 4096;

        /** @brief Mascara binaria para extraer el indice de bucket del hash. */
        static constexpr uint32_t BUCKET_MASK = BUCKETS - 1;

        WaitTable() noexcept;
        ~WaitTable() noexcept;

        WaitTable(const WaitTable &) = delete;
        WaitTable &operator=(const WaitTable &) = delete;
        WaitTable(WaitTable &&)                 = delete;
        WaitTable &operator=(WaitTable &&)      = delete;

        /**
         * @brief Anyade @p encoded_pid al final de la cola @c (handle, kind).
         *
         * Lock-free a nivel global: solo el bucket correspondiente toma
         * spinlock.  Si dos threads llaman @c push sobre handles en
         * buckets distintos, no se bloquean entre si.
         *
         * @param handle      Handle del objeto cuya cola recibe el waiter.
         * @param kind        Tipo de cola (MONITOR o CONDVAR).
         * @param encoded_pid PID empacado del proceso que espera.
         */
        void push(uint32_t handle, WaitKind kind, uint64_t encoded_pid);

        /**
         * @brief Extrae el primer @c encoded_pid de la cola.  FIFO order.
         *
         * @return @c encoded_pid del primer waiter, o 0 si la cola esta vacia.
         */
        uint64_t pop_one(uint32_t handle, WaitKind kind);

        /**
         * @brief Drena la cola completa y devuelve todos los waiters.
         *
         * El orden es FIFO (mismo orden que push).  La cola queda vacia
         * tras la llamada.  Util para notifyAll: el caller wake-uppea
         * todos sin tener que bloquear el bucket en bucle.
         *
         * @return Vector con @c encoded_pid en orden FIFO (vacio si empty).
         */
        std::vector<uint64_t> pop_all(uint32_t handle, WaitKind kind);

        /**
         * @brief Numero total de waiters en toda la tabla.  Diagnostico.
         *
         * NO es atomic-snapshot: itera cada bucket bajo su spinlock; el
         * total puede estar desfasado bajo contencion concurrente.  Util
         * para reporting / leaks debugging, no para correctness.
         */
        size_t total_waiters() const noexcept;

    private:
        /**
         * @brief Entrada dentro de un bucket: cola por @c (handle, kind).
         *
         * Multiples entries pueden vivir en el mismo bucket si sus
         * handles colisionan en hash.  Chain enlazada (singly-linked).
         */
        struct Entry {
            uint32_t              handle;  ///< Handle GC del objeto
            WaitKind              kind;    ///< MONITOR o CONDVAR
            uint8_t               _pad[3]; ///< alineacion (queue alineada a 8)
            std::vector<uint64_t> queue;   ///< FIFO de encoded_pids
            Entry                *next;    ///< siguiente en la chain del bucket
        };

        /**
         * @brief Bucket cache-line aligned con spinlock + chain de entries.
         *
         * @c std::atomic_flag es el spinlock mas ligero del estandar C++
         * (test_and_set + clear).  Garantiza fairness razonable bajo
         * contencion porque cada thread spin-waitea localmente sin
         * involucrar el OS scheduler.
         */
        struct alignas(64) Bucket {
            mutable std::atomic_flag lock; ///< spinlock (1 byte funcional)
            Entry                   *chain; ///< primer entry de la chain (NULL si vacio)
            // El @c alignas(64) garantiza que sizeof(Bucket) sea multiplo
            // de 64 (el compilador rellena al final automaticamente).
            // No necesitamos @c _pad explicito; con @c std::atomic_flag (1B)
            // + implicit pad (7B) + Entry* (8B) = 16B utiles, el compilador
            // anyade 48B de cola para alcanzar exactamente 64.
        };
        // Verificacion ABI/cache: cada bucket debe ocupar exactamente
        // 64 bytes para que dos buckets consecutivos NO compartan
        // cache line (false sharing seria catastrofico).
        static_assert(sizeof(Bucket) == 64,
                      "Bucket debe ocupar exactamente 64 bytes");
        static_assert(alignof(Bucket) == 64,
                      "Bucket debe estar alineado a cache line");

        Bucket buckets_[BUCKETS]; ///< 4096 * 64 B = 256 KB

        /**
         * @brief Calcula el indice de bucket para @c (handle, kind).
         *
         * Hash multiplicativo Fibonacci-style + mezcla con kind.  Mantiene
         * los handles consecutivos en buckets dispersos (evita clustering
         * cuando se alocan objetos en rafaga).
         */
        static inline uint32_t hash_bucket(uint32_t handle, WaitKind kind) noexcept {
            // Magic Fibonacci hashing constant (golden ratio * 2^32)
            uint32_t k = static_cast<uint32_t>(kind);
            return ((handle * 0x9E3779B9u) ^ (k << 16)) & BUCKET_MASK;
        }

        // ----------------------------------------------------------------
        // Helpers internos (bajo lock del bucket)
        // ----------------------------------------------------------------

        /**
         * @brief Busca la @c Entry para @c (handle, kind) en el bucket.
         *
         * Caller debe tener el spinlock del bucket.  Devuelve nullptr
         * si no existe.  O(N_entries_en_bucket); tipicamente 0 o 1.
         */
        static Entry *find_entry(Bucket &b, uint32_t handle, WaitKind kind) noexcept;

        /**
         * @brief Adquiere el spinlock del bucket con backoff suave.
         *
         * Spin-busy hasta que @c test_and_set devuelva false.  En CPUs
         * modernas, el primer test es ~3 ns; bajo contencion, cada
         * retry usa @c PAUSE (x86) o @c YIELD (ARM) para no quemar
         * issue ports.
         */
        static inline void lock_bucket(Bucket &b) noexcept {
            while (b.lock.test_and_set(std::memory_order_acquire)) {
                // Backoff: hint al CPU de que estamos en spin loop.
                // Reduce contention en el bus de cache.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
                __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ __volatile__("yield");
#endif
            }
        }

        /** @brief Libera el spinlock del bucket. */
        static inline void unlock_bucket(Bucket &b) noexcept {
            b.lock.clear(std::memory_order_release);
        }
    };

} // namespace gc
