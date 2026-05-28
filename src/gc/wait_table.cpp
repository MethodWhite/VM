/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file wait_table.cpp
 * @brief Implementacion del WaitTable lock-free per-bucket (Phase Z.4).
 *
 * Ver @c include/gc/wait_table.h para el contrato y el disenyo.
 *
 * Notas de implementacion
 * =======================
 * - **Allocacion de Entries**: usamos @c new/@c delete C++ estandar.
 *   El coste de @c new (~30-50 ns en glibc / Windows) es despreciable
 *   comparado con el coste de bloqueo bytecode + scheduler que viene
 *   inmediatamente despues.  Phase Z+ puede migrar a un pool dedicado
 *   con free list lock-free si profile lo demanda.
 *
 * - **Memory ordering del spinlock**: test_and_set con acquire publica
 *   los reads posteriores tras el lock; clear con release publica los
 *   writes anteriores antes de soltar.  Sincronizacion estandar.
 *
 * - **FIFO via vector**: @c push_back + @c erase(begin) da O(N) amortizado
 *   en notify-heavy patterns.  En la practica, las colas son cortas
 *   (1-10 elementos) por lo que vector es preferible a deque/list por
 *   localidad de cache.
 *
 * - **Cleanup en pop**: cuando una entry queda con queue vacia tras
 *   pop_one, la borramos del chain del bucket para evitar leak.  Esto
 *   evita que un objeto que tuvo waiters acumule entries vacios.
 */

#include "gc/wait_table.h"

namespace gc {

    // ------------------------------------------------------------------
    // Constructor / destructor
    // ------------------------------------------------------------------

    WaitTable::WaitTable() noexcept {
        // Inicializar los 4096 buckets.  El @c std::atomic_flag NO tiene
        // un constructor que tome valor inicial en C++17 (ATOMIC_FLAG_INIT
        // se deprecio en C++20); usamos @c clear para garantizar estado
        // "unlocked" desde el inicio.
        for (uint32_t i = 0; i < BUCKETS; ++i) {
            buckets_[i].lock.clear(std::memory_order_relaxed);
            buckets_[i].chain = nullptr;
        }
    }

    WaitTable::~WaitTable() noexcept {
        // Liberar todas las entries en cada bucket.  En este punto no
        // hay concurrencia (estamos en el dtor), asi que no necesitamos
        // tomar el lock.
        for (uint32_t i = 0; i < BUCKETS; ++i) {
            Entry *e = buckets_[i].chain;
            while (e != nullptr) {
                Entry *next = e->next;
                delete e;
                e = next;
            }
            buckets_[i].chain = nullptr;
        }
    }

    // ------------------------------------------------------------------
    // Helper: buscar entry para (handle, kind) en un bucket (bajo lock)
    // ------------------------------------------------------------------

    WaitTable::Entry *WaitTable::find_entry(Bucket &b,
                                            uint32_t handle,
                                            WaitKind kind) noexcept {
        // Linear scan del chain del bucket.  Tipicamente 0 o 1 entries.
        // Si hay >1, es porque dos handles distintos colisionaron en hash.
        for (Entry *e = b.chain; e != nullptr; e = e->next) {
            if (e->handle == handle && e->kind == kind) {
                return e;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // push: anyadir waiter al final de la cola (handle, kind)
    // ------------------------------------------------------------------

    void WaitTable::push(uint32_t handle, WaitKind kind, uint64_t encoded_pid) {
        Bucket &b = buckets_[hash_bucket(handle, kind)];
        lock_bucket(b);

        Entry *e = find_entry(b, handle, kind);
        if (e == nullptr) {
            // Crear nueva entry y prependerla al chain.
            // Prepend (no append) es O(1); el chain order no importa
            // porque buscamos por (handle, kind), no por orden.
            e          = new Entry{};
            e->handle  = handle;
            e->kind    = kind;
            e->next    = b.chain;
            b.chain    = e;
        }
        // FIFO push: anyadir al final de la cola.
        e->queue.push_back(encoded_pid);

        unlock_bucket(b);
    }

    // ------------------------------------------------------------------
    // pop_one: extraer primer waiter (FIFO)
    // ------------------------------------------------------------------

    uint64_t WaitTable::pop_one(uint32_t handle, WaitKind kind) {
        Bucket &b = buckets_[hash_bucket(handle, kind)];
        lock_bucket(b);

        Entry *e = find_entry(b, handle, kind);
        if (e == nullptr || e->queue.empty()) {
            unlock_bucket(b);
            // BugFix Z.6: sentinel "no waiter" debe ser UINT64_MAX, NO 0,
            // porque PID encoded = (sched_id<<32)|local_pid PUEDE ser 0
            // (caso: main process en scheduler 0).  Mismo fix que en
            // @Async A.7.3 fase 1 para shared_futures.
            return UINT64_MAX;
        }

        uint64_t pid = e->queue.front();
        e->queue.erase(e->queue.begin()); // FIFO: pop del frente

        // Si la cola quedo vacia, eliminar la entry del bucket chain
        // para evitar leak de entries-zombie acumuladas.
        if (e->queue.empty()) {
            // Buscar el predecesor en el chain y unlink.
            Entry *prev = nullptr;
            for (Entry *cur = b.chain; cur != nullptr; cur = cur->next) {
                if (cur == e) {
                    if (prev) prev->next = e->next;
                    else      b.chain    = e->next;
                    delete e;
                    break;
                }
                prev = cur;
            }
        }

        unlock_bucket(b);
        return pid;
    }

    // ------------------------------------------------------------------
    // pop_all: drenar cola completa en orden FIFO
    // ------------------------------------------------------------------

    std::vector<uint64_t> WaitTable::pop_all(uint32_t handle, WaitKind kind) {
        Bucket &b = buckets_[hash_bucket(handle, kind)];
        lock_bucket(b);

        Entry *e = find_entry(b, handle, kind);
        if (e == nullptr) {
            unlock_bucket(b);
            return {};
        }

        // Mover el vector entero (sin copia) y luego unlink la entry.
        std::vector<uint64_t> result = std::move(e->queue);

        // Unlink + delete: la entry queda inutil tras vaciarla.
        Entry *prev = nullptr;
        for (Entry *cur = b.chain; cur != nullptr; cur = cur->next) {
            if (cur == e) {
                if (prev) prev->next = e->next;
                else      b.chain    = e->next;
                delete e;
                break;
            }
            prev = cur;
        }

        unlock_bucket(b);
        return result;
    }

    // ------------------------------------------------------------------
    // total_waiters: snapshot diagnostico (NO atomic globalmente)
    // ------------------------------------------------------------------

    size_t WaitTable::total_waiters() const noexcept {
        size_t total = 0;
        for (uint32_t i = 0; i < BUCKETS; ++i) {
            Bucket &b = const_cast<Bucket &>(buckets_[i]);
            lock_bucket(b);
            for (Entry *e = b.chain; e != nullptr; e = e->next) {
                total += e->queue.size();
            }
            unlock_bucket(b);
        }
        return total;
    }

} // namespace gc
