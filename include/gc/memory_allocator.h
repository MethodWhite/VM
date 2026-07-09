#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <cstdint>
#include <cstddef>

namespace vm { class ArenaManager; }

namespace gc {

/**
 * @brief Permisos de memoria para @c allocate().
 */
enum class MemPerm : uint8_t {
    READ  = 1,
    WRITE = 2,
    EXEC  = 4,
};

inline MemPerm operator|(MemPerm a, MemPerm b) {
    return static_cast<MemPerm>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool operator&(MemPerm a, MemPerm b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

/**
 * @brief Resultado de una asignacion: puntero + handle opaco para liberar.
 */
struct MemBlock {
    uint8_t  *ptr; ///< Puntero al bloque asignado (nullptr si fallo).
    uint64_t  id;  ///< Handle opaco para free() especifico del backend.
};

/**
 * @brief Interface abstracta para asignacion de memoria del GC.
 *
 * Permite que GcHeap funcione con diferentes backends de memoria:
 *   - ArenaManager (produccion, memoria virtual con permisos)
 *   - std::malloc (tests, sin dependencia del VM)
 */
class MemoryAllocator {
public:
    virtual ~MemoryAllocator() = default;

    /**
     * @brief Reserva un bloque de @p size bytes con los permisos indicados.
     */
    virtual MemBlock allocate(size_t size, MemPerm perms) = 0;

    /**
     * @brief Libera un bloque previamente asignado.
     * @param block El bloque devuelto por allocate().
     */
    virtual void free(MemBlock block) = 0;
};

/**
 * @brief Implementacion de MemoryAllocator basada en ArenaManager.
 *
 * Usa el sistema de memoria virtual del VM (mmap/VirtualAlloc).
 * Es el allocator por defecto para el GcHeap en produccion.
 */
class ArenaMemoryAllocator : public MemoryAllocator {
public:
    explicit ArenaMemoryAllocator(vm::ArenaManager &arena_mgr);
    ~ArenaMemoryAllocator() override = default;

    MemBlock allocate(size_t size, MemPerm perms) override;
    void free(MemBlock block) override;

private:
    vm::ArenaManager &arena_mgr_;
};

/**
 * @brief Implementacion de MemoryAllocator basada en std::malloc.
 *
 * Util para tests y entornos sin sistema de memoria virtual.
 * Los permisos son ignorados (malloc no los soporta).
 */
class MallocMemoryAllocator : public MemoryAllocator {
public:
    MemBlock allocate(size_t size, MemPerm /*perms*/) override;
    void free(MemBlock block) override;
};

} // namespace gc

#endif // MEMORY_ALLOCATOR_H
