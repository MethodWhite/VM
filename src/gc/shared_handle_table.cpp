/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file shared_handle_table.cpp
 * @brief Implementacion del SharedHandleTable lock-free (Phase Z.5).
 *
 * Ver @c include/gc/shared_handle_table.h para el contrato y el disenyo.
 *
 * Notas
 * =====
 * - El indice 0 NUNCA se aloca (esta reservado como SHARED_NULL_HANDLE).
 *   El alloc empieza desde el indice 1.
 *
 * - El free list intrusivo es una pila Treiber tagged-ptr ABA-safe.
 *   La cabeza @c free_list_head_ empaca:
 *     bits 0-31  -> indice del primer slot libre (o 0 si lista vacia)
 *     bits 32-47 -> tag ABA
 *     bits 48-63 -> reservado (0)
 *   Cuando el free list esta vacio, los nuevos slots se obtienen via
 *   @c next_fresh_handle_ fetch_add (alocacion incremental, sin reuso).
 *
 * - El primer chunk se aloca en el constructor; los siguientes se
 *   alocan lazy via @c alloc_chunk + CAS double-check pattern.
 */

#include "gc/shared_handle_table.h"

#include "arena/arena.h"

#include <cstring>
#include <cstdlib>   // calloc / free para mark bitmaps lazy

namespace gc {

    namespace {
        // Helpers para empacar/desempacar la cabeza del free list.
        // Layout: bits 0-31 idx; bits 32-47 tag; bits 48-63 reservados.
        constexpr uint64_t FL_IDX_MASK = 0x00000000FFFFFFFFULL;
        constexpr uint64_t FL_TAG_MASK = 0x0000FFFF00000000ULL;
        constexpr int      FL_TAG_SHIFT = 32;

        inline uint32_t fl_idx(uint64_t head) noexcept {
            return static_cast<uint32_t>(head & FL_IDX_MASK);
        }
        inline uint16_t fl_tag(uint64_t head) noexcept {
            return static_cast<uint16_t>((head & FL_TAG_MASK) >> FL_TAG_SHIFT);
        }
        inline uint64_t fl_pack(uint32_t idx, uint16_t tag) noexcept {
            return static_cast<uint64_t>(idx)
                 | (static_cast<uint64_t>(tag) << FL_TAG_SHIFT);
        }
    } // namespace

    // ------------------------------------------------------------------
    // Constructor / destructor
    // ------------------------------------------------------------------

    SharedHandleTable::SharedHandleTable() noexcept
        : free_list_head_(0),
          // Slot 0 reservado como SHARED_NULL_HANDLE; allocs empiezan en 1.
          next_fresh_handle_(1),
          num_chunks_(0),
          live_count_(0),
          init_ok_(false) {
        // Zero-init de los punteros a chunks
        for (uint32_t i = 0; i < MAX_CHUNKS; ++i) {
            chunks_[i].store(nullptr, std::memory_order_relaxed);
            // Phase Z.10 ext: mark bitmaps lazy.  nullptr = "no marcas
            // poblados" (interpretado como todos unmarked).
            mark_chunks_[i].store(nullptr, std::memory_order_relaxed);
        }
        // Alocar el primer chunk eager (los siguientes son lazy)
        init_ok_ = alloc_chunk(0);
    }

    SharedHandleTable::~SharedHandleTable() noexcept {
        // Liberar todos los chunks alocados
        uint32_t n = num_chunks_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            HandleSlot *chunk = chunks_[i].load(std::memory_order_relaxed);
            if (chunk) {
                vm::free_memory(chunk,
                                static_cast<size_t>(CHUNK_SLOTS) * sizeof(HandleSlot));
                chunks_[i].store(nullptr, std::memory_order_relaxed);
            }
            // Liberar bitmap de marca si fue alocado.
            uint64_t *mark = mark_chunks_[i].load(std::memory_order_relaxed);
            if (mark) {
                std::free(mark);
                mark_chunks_[i].store(nullptr, std::memory_order_relaxed);
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase Z.10 ext: mark/sweep bitmap helpers.
    // ------------------------------------------------------------------

    void SharedHandleTable::clear_marks() noexcept {
        // Para cada chunk vivo, asegurar que el bitmap existe y zero-init.
        uint32_t n = num_chunks_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            if (chunks_[i].load(std::memory_order_relaxed) == nullptr) continue;
            uint64_t *mark = mark_chunks_[i].load(std::memory_order_acquire);
            if (mark == nullptr) {
                // Lazy alloc: 16384 bits = 256 u64s = 2 KB/chunk.
                size_t bytes = static_cast<size_t>(MARK_WORDS_PER_CHUNK) * sizeof(uint64_t);
                mark = static_cast<uint64_t *>(std::calloc(MARK_WORDS_PER_CHUNK, sizeof(uint64_t)));
                if (!mark) continue; // OOM: el slot queda no-marcable, conservativo (sweep no lo barre)
                uint64_t *expected = nullptr;
                if (!mark_chunks_[i].compare_exchange_strong(
                        expected, mark,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    std::free(mark); // race
                    mark = expected;
                }
                continue; // ya esta zero-init del calloc
            }
            // Zero-init existing bitmap.
            std::memset(mark, 0, static_cast<size_t>(MARK_WORDS_PER_CHUNK) * sizeof(uint64_t));
        }
    }

    void SharedHandleTable::mark(uint32_t handle) noexcept {
        uint32_t idx = handle & SHARED_HANDLE_MASK;
        if (idx == 0) return;
        uint32_t chunk_idx = idx / CHUNK_SLOTS;
        uint32_t slot_in_chunk = idx % CHUNK_SLOTS;
        if (chunk_idx >= MAX_CHUNKS) return;
        uint64_t *mark = mark_chunks_[chunk_idx].load(std::memory_order_acquire);
        if (!mark) return; // clear_marks no se ha llamado (no es GC active)
        uint32_t word_idx = slot_in_chunk / 64;
        uint32_t bit_idx  = slot_in_chunk % 64;
        mark[word_idx] |= (1ULL << bit_idx);
    }

    bool SharedHandleTable::is_marked(uint32_t handle) const noexcept {
        uint32_t idx = handle & SHARED_HANDLE_MASK;
        if (idx == 0) return false;
        uint32_t chunk_idx = idx / CHUNK_SLOTS;
        uint32_t slot_in_chunk = idx % CHUNK_SLOTS;
        if (chunk_idx >= MAX_CHUNKS) return false;
        uint64_t *mark = mark_chunks_[chunk_idx].load(std::memory_order_acquire);
        if (!mark) return false;
        uint32_t word_idx = slot_in_chunk / 64;
        uint32_t bit_idx  = slot_in_chunk % 64;
        return (mark[word_idx] >> bit_idx) & 1ULL;
    }

    // ------------------------------------------------------------------
    // Alloc lazy de un chunk
    // ------------------------------------------------------------------

    bool SharedHandleTable::alloc_chunk(uint32_t chunk_idx) noexcept {
        if (chunk_idx >= MAX_CHUNKS) return false;

        // Double-check sin carrera: si otro thread acaba de alocar el
        // chunk, no duplicamos.  El CAS final sella la decision.
        HandleSlot *existing = chunks_[chunk_idx].load(std::memory_order_acquire);
        if (existing != nullptr) return true;

        size_t bytes = static_cast<size_t>(CHUNK_SLOTS) * sizeof(HandleSlot);
        void *raw = vm::allocate_memory(bytes,
                                         vm::MemPerm::READ | vm::MemPerm::WRITE);
        if (!raw) return false;

        // Inicializar todos los slots como libres (host_ptr=0, size=0)
        HandleSlot *chunk = static_cast<HandleSlot *>(raw);
        std::memset(chunk, 0, bytes); // host_ptr=0, next_free=0, size=0

        // CAS: si otro thread gano la carrera, liberar nuestro chunk
        HandleSlot *expected = nullptr;
        if (!chunks_[chunk_idx].compare_exchange_strong(
                expected, chunk,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Otro thread ya alocaba este chunk; usar el suyo
            vm::free_memory(chunk, bytes);
            return true;
        }
        // Avanzar el contador de chunks visibles (monotonico)
        uint32_t cur_n = num_chunks_.load(std::memory_order_relaxed);
        while (cur_n <= chunk_idx) {
            if (num_chunks_.compare_exchange_weak(
                    cur_n, chunk_idx + 1,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // register_object: aloca slot + escribe host_ptr
    // ------------------------------------------------------------------

    uint32_t SharedHandleTable::register_object(uint8_t *host_ptr,
                                                  uint32_t size_bytes) noexcept {
        if (!host_ptr) return SHARED_NULL_HANDLE;

        // Fast path: pop del free list (Treiber con ABA tag).
        uint64_t head = free_list_head_.load(std::memory_order_acquire);
        for (;;) {
            uint32_t idx = fl_idx(head);
            if (idx == 0) break; // free list vacio: caer al path "fresh alloc"

            HandleSlot *slot = slot_at(idx);
            if (!slot) break; // chunk no alocado (raro): caer a fresh alloc

            // Leer next_free del slot (libre); relaxed porque la
            // sincronizacion la hace el CAS del head.
            uint32_t next = slot->next_free.load(std::memory_order_relaxed);
            uint16_t new_tag = static_cast<uint16_t>(fl_tag(head) + 1);
            uint64_t new_head = fl_pack(next, new_tag);

            if (free_list_head_.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // Slot obtenido: escribir host_ptr + size; clear next_free
                slot->host_ptr.store(reinterpret_cast<uint64_t>(host_ptr),
                                      std::memory_order_release);
                slot->next_free.store(0, std::memory_order_relaxed);
                slot->size_bytes = size_bytes;
                live_count_.fetch_add(1, std::memory_order_relaxed);
                return idx | SHARED_HANDLE_BIT; // tag con bit 31
            }
            // CAS fallo: head se actualizo; retry
        }

        // Slow path: fresh alloc via next_fresh_handle_
        for (;;) {
            uint32_t idx = next_fresh_handle_.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= MAX_HANDLES) {
                // Limite global alcanzado
                next_fresh_handle_.fetch_sub(1, std::memory_order_relaxed); // rollback
                return SHARED_NULL_HANDLE;
            }
            uint32_t chunk_idx = idx / CHUNK_SLOTS;
            // Asegurar que el chunk esta alocado
            if (chunks_[chunk_idx].load(std::memory_order_acquire) == nullptr) {
                if (!alloc_chunk(chunk_idx)) {
                    // OOM al crecer
                    return SHARED_NULL_HANDLE;
                }
            }
            HandleSlot *slot = slot_at(idx);
            if (!slot) return SHARED_NULL_HANDLE; // imposible salvo bug
            slot->host_ptr.store(reinterpret_cast<uint64_t>(host_ptr),
                                  std::memory_order_release);
            slot->next_free.store(0, std::memory_order_relaxed);
            slot->size_bytes = size_bytes;
            live_count_.fetch_add(1, std::memory_order_relaxed);
            return idx | SHARED_HANDLE_BIT;
        }
    }

    // ------------------------------------------------------------------
    // lookup: atomic load del host_ptr
    // ------------------------------------------------------------------

    uint8_t *SharedHandleTable::lookup(uint32_t handle) const noexcept {
        uint32_t idx = handle & SHARED_HANDLE_MASK;
        if (idx == 0) return nullptr; // SHARED_NULL_HANDLE
        HandleSlot *slot = slot_at(idx);
        if (!slot) return nullptr;
        uint64_t p = slot->host_ptr.load(std::memory_order_acquire);
        return reinterpret_cast<uint8_t *>(p);
    }

    // ------------------------------------------------------------------
    // unregister: clear slot + push a free list
    // ------------------------------------------------------------------

    void SharedHandleTable::unregister(uint32_t handle) noexcept {
        uint32_t idx = handle & SHARED_HANDLE_MASK;
        if (idx == 0) return;
        HandleSlot *slot = slot_at(idx);
        if (!slot) return;

        // Verificar que el slot estaba ocupado (paranoia: si ya esta libre,
        // un double-free crea un ciclo en el free list).
        uint64_t cur = slot->host_ptr.load(std::memory_order_relaxed);
        if (cur == 0) return; // ya libre: no-op idempotente

        // Limpiar host_ptr ANTES de pushear al free list para que un
        // lookup concurrente no devuelva el ptr stale.
        slot->host_ptr.store(0, std::memory_order_release);
        slot->size_bytes = 0;

        // Push al free list con CAS + tag ABA
        uint64_t head = free_list_head_.load(std::memory_order_relaxed);
        for (;;) {
            slot->next_free.store(fl_idx(head), std::memory_order_relaxed);
            uint16_t new_tag = static_cast<uint16_t>(fl_tag(head) + 1);
            uint64_t new_head = fl_pack(idx, new_tag);
            if (free_list_head_.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        live_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------
    // Stats
    // ------------------------------------------------------------------

    uint32_t SharedHandleTable::live_count() const noexcept {
        return live_count_.load(std::memory_order_relaxed);
    }

    uint32_t SharedHandleTable::chunk_count() const noexcept {
        return num_chunks_.load(std::memory_order_relaxed);
    }

} // namespace gc
