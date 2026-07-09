#include "gc/memory_allocator.h"
#include "arena/arena_manager.h"
#include "arena/arena.h"

namespace gc {

// =========================================================================
// ArenaMemoryAllocator
// =========================================================================

ArenaMemoryAllocator::ArenaMemoryAllocator(vm::ArenaManager &arena_mgr)
    : arena_mgr_(arena_mgr) {}

MemBlock ArenaMemoryAllocator::allocate(size_t size, MemPerm perms) {
    // Convertir gc::MemPerm a vm::MemPerm
    vm::MemPerm vm_perms = vm::MemPerm::NONE;
    if (perms & MemPerm::READ)  vm_perms = vm_perms | vm::MemPerm::READ;
    if (perms & MemPerm::WRITE) vm_perms = vm_perms | vm::MemPerm::WRITE;
    if (perms & MemPerm::EXEC)  vm_perms = vm_perms | vm::MemPerm::EXEC;

    uint64_t id = arena_mgr_.create_arena(size, vm_perms);
    if (id == 0) return {nullptr, 0};

    const vm::Arena *arena = arena_mgr_.get_arena(id);
    if (!arena) {
        arena_mgr_.free_arena(id);
        return {nullptr, 0};
    }

    return {static_cast<uint8_t *>(arena->ptr), id};
}

void ArenaMemoryAllocator::free(MemBlock block) {
    if (block.ptr && block.id != 0) {
        arena_mgr_.free_arena(block.id);
    }
}

// =========================================================================
// MallocMemoryAllocator
// =========================================================================

MemBlock MallocMemoryAllocator::allocate(size_t size, MemPerm /*perms*/) {
    uint8_t *ptr = static_cast<uint8_t *>(std::malloc(size));
    return {ptr, reinterpret_cast<uint64_t>(ptr)};
}

void MallocMemoryAllocator::free(MemBlock block) {
    if (block.ptr) {
        std::free(block.ptr);
    }
}

} // namespace gc
