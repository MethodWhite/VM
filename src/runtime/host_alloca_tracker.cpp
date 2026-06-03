/**
 * @file host_alloca_tracker.cpp
 * @brief Cleanup de allocs host por-frame (Sprint MMM-ext leak-fix).
 */

#include "runtime/host_alloca_tracker.h"
#include "runtime/proceso_runtime.h"

namespace runtime {

    void host_alloca_release_all(ProcessVM *vm, loader::FrameHeader *frame) {
        if (frame == nullptr) return;
        auto *list = static_cast<std::vector<uint8_t*> *>(frame->host_allocas);
        if (list == nullptr) return;
        /* Liberar en LIFO (orden inverso al alloc).  RawAllocator::free
         * es null-safe y tolera ptrs no alocados via su path; aun asi
         * filtramos nullptr defensivamente. */
        for (auto it = list->rbegin(); it != list->rend(); ++it) {
            if (*it != nullptr) {
                vm->raw_alloc.free(reinterpret_cast<uint64_t>(*it));
            }
        }
        delete list;
        frame->host_allocas = nullptr;
    }

} // namespace runtime
