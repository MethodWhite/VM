/**
 * @file host_alloca_tracker.h
 * @brief Helpers para tracking + cleanup de allocs host por-frame.
 *
 * Sprint MMM-ext leak-fix (2026-06-01).  Cuando el IR pass
 * `ir_pass_promote_callned_allocas` marca un ALLOCA con
 * `host_alloca=true`, el bytecode emit del interp invoca
 * `host_alloca_track(frame, ptr)` para registrar el ptr en el frame
 * actual.  En RET y en `do_throw` (al pop un frame), invoke
 * `host_alloca_release_all(frame)` para liberar todos los ptrs del
 * frame con `RawAllocator::free`.
 *
 * El campo @c FrameHeader::host_allocas es @c void* opaco para no
 * arrastrar STL dentro de @c oop_types.h ; lo casteamos aqui a
 * @c std::vector<uint8_t*> bajo demanda.
 */

#ifndef VESTA_RUNTIME_HOST_ALLOCA_TRACKER_H
#define VESTA_RUNTIME_HOST_ALLOCA_TRACKER_H

#include <cstdint>
#include <vector>

#include "loader/oop_types.h"

namespace runtime {
    class ProcessVM;
}

namespace runtime {

    /**
     * @brief Registra @p ptr para cleanup al destruir @p frame.
     *
     * Lazy: aloca el vector solo cuando se registra al menos un ptr.
     * Cero overhead para frames que NO contienen host_allocas (la
     * mayoria, ya que el flag `host_alloca` viene del IR pass de
     * auto-promote sobre ALLOCAs que fluyen a CALLN).
     */
    inline void host_alloca_track(loader::FrameHeader *frame, uint8_t *ptr) {
        if (frame == nullptr || ptr == nullptr) return;
        auto *list = static_cast<std::vector<uint8_t*> *>(frame->host_allocas);
        if (list == nullptr) {
            list = new std::vector<uint8_t*>();
            frame->host_allocas = list;
        }
        list->push_back(ptr);
    }

    /**
     * @brief Libera todos los host_allocas registrados en @p frame con
     * @c RawAllocator::free.  Idempotente: tras la primera invocacion el
     * campo queda en nullptr y nuevas llamadas son no-op.
     *
     * Invocada por @c exec_instr_ret y @c do_throw (ANTES de devolver
     * el frame al @c frame_pool).
     */
    void host_alloca_release_all(ProcessVM *vm, loader::FrameHeader *frame);

} // namespace runtime

#endif // VESTA_RUNTIME_HOST_ALLOCA_TRACKER_H
