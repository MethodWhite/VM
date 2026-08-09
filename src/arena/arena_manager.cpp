/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file arena_manager.cpp
 * @brief Implementacion del gestor global de arenas de memoria de VestaVM.
 *
 * Implementa @c ArenaManager: asignacion y liberacion de bloques de memoria
 * mapeados, conversion de MappedPtr a cadena de texto y volcado del estado
 * del gestor para depuracion.
 */
#include "arena/arena_manager.h"

#include <iomanip>
#include <vector>

namespace vm {

    /**
     * @brief Construye una cadena de texto con el volcado de los tres campos del MappedPtr.
     *
     * Muestra en hexadecimal de 16 digitos:
     *   - Virtual:  el valor bruto de ptr_vm.raw (direccion virtual)
     *   - Mapped:   el valor bruto de mapped.ptr_vm.raw (direccion virtual mapeada)
     *   - Host:     el puntero real del proceso anfitrion
     *
     * @return Cadena con el volcado formateado.
     */
    std::string MappedPtr::to_string() const {
        std::ostringstream ss;
        ss << "MappedPtrHost {\n";

        // imprimir direccion virtual en hexadecimal de 16 digitos
        ss << "  Virtual:  0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr_vm.raw)) << std::dec << "\n";

        // imprimir direccion virtual mapeada (puede ser un vm_map_ptr)
        ss << "  Mapped:   0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mapped.ptr_vm.raw)) << std::dec << "\n";

        // imprimir puntero de host en hexadecimal de 16 digitos
        ss << "  Host:     0x" << std::hex << std::setw(16) << std::setfill('0')
                << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mapped.ptr_host)) << std::dec << "\n";
        ss << "}\n";
        return ss.str();
    }

    /**
     * @brief Imprime el volcado del MappedPtr en el flujo indicado.
     *
     * Delega en to_string() para no duplicar la logica de formato.
     *
     * @param os Flujo de salida donde se escribe el volcado.
     */
    void MappedPtr::print(std::ostream &os) const {
        os << to_string(); // reutilizar la cadena generada por to_string
    }

    /**
     * @brief Inicializa el gestor de arenas con todos los contadores a cero.
     *
     * El mapa de arenas comienza vacio y next_id se pone a 1 para que el
     * primer bloque creado reciba el identificador 1: el ID 0 esta
     * RESERVADO como valor de error (ver create_arena), asi una arena
     * valida jamas puede tener ID 0.
     */
    ArenaManager::ArenaManager()
        : total_allocated_bytes_(0), // ninguna arena reservada todavia
          next_id(1),                // el ID 0 es el valor de error
          arenas()                   // mapa de arenas vacio
    {
    }

    /**
     * @brief Libera todos los bloques de memoria al destruir el gestor.
     *
     * Llama a free_all() para garantizar que no queden arenas activas
     * tras la destruccion del objeto.
     */
    ArenaManager::~ArenaManager() {
        free_all(); // liberar todo lo pendiente antes de destruir
    }

    /**
     * @brief Reserva un nuevo bloque de memoria y lo registra en el catalogo.
     *
     * Llama a allocate_memory() para obtener memoria del sistema operativo
     * con los permisos solicitados.  Si la asignacion falla imprime un
     * mensaje de error y devuelve 0.
     *
     * @param size  Tamanyo del bloque en bytes (se redondea a pagina internamente).
     * @param perms Permisos de acceso (READ / WRITE / EXEC o combinaciones).
     * @return      ID unico del bloque creado, o 0 si la asignacion falla.
     */
    uint64_t ArenaManager::create_arena(size_t size, MemPerm perms) {
        void *mem = allocate_memory(size, perms); // reservar memoria del SO
        if (!mem) {
            std::cerr << "[ArenaManager] Error al asignar memoria\n"; // informar del fallo
            return 0; // ID 0 indica error
        }

        uint64_t id = next_id++;             // asignar ID unico y avanzar el contador
        arenas[id] = Arena{mem, size, perms}; // registrar la arena en el catalogo
        total_allocated_bytes_ += size;       // actualizar el total de bytes reservados
        return id;
    }

    /**
     * @brief Libera el bloque identificado por @p id y lo elimina del catalogo.
     *
     * Resta el tamanyo de la arena del acumulador total, libera la memoria
     * mediante free_memory() y borra la entrada del mapa.
     *
     * @param id Identificador del bloque a liberar.
     * @return   true si el bloque existia y fue liberado; false si no existia.
     */
    bool ArenaManager::free_arena(uint64_t id) {
        auto it = arenas.find(id); // buscar la arena por ID
        if (it == arenas.end())
            return false; // no existe, nada que liberar

        total_allocated_bytes_ -= it->second.size; // descontar bytes del total
        free_memory(it->second.ptr, it->second.size); // liberar memoria del SO
        arenas.erase(it); // eliminar del catalogo
        return true;
    }

    /**
     * @brief Devuelve un puntero de solo lectura a la arena indicada.
     *
     * No transfiere propiedad.  El puntero puede quedar invalidado
     * si se llama a free_arena() o free_all() con el mismo ID.
     *
     * @param id Identificador del bloque a consultar.
     * @return   Puntero constante a la Arena, o nullptr si el ID no existe.
     */
    const Arena *ArenaManager::get_arena(uint64_t id) const {
        auto it = arenas.find(id); // buscar por ID en el mapa
        if (it == arenas.end()) return nullptr; // ID inexistente
        return &it->second; // devolver puntero a la arena sin ceder propiedad
    }

    /**
     * @brief Libera todas las arenas registradas en el catalogo.
     *
     * Recoge primero todos los IDs en un vector para evitar invalidar
     * el iterador del mapa durante el borrado.  Tras la llamada el mapa
     * queda vacio y total_allocated_bytes_ vale cero.
     */
    void ArenaManager::free_all() {
        if (arenas.empty()) return; // nada que liberar

        // recoger IDs en vector para no invalidar el iterador durante el borrado
        std::vector<int> ids;
        for (const auto &[id, arena]: arenas) {
            ids.push_back(id); // acumular IDs activos
        }

        for (int id: ids) {
            free_arena(id); // liberar cada arena por su ID
        }
    }

    /**
     * @brief Busca el ID de la arena cuyo puntero de host coincide con @p host_ptr.
     *
     * Realiza una busqueda lineal O(N) sobre el mapa de arenas.
     * Solo debe usarse fuera del hot path (p.ej. durante unmap).
     *
     * @todo Anadir un indice inverso para reducir la complejidad a O(1).
     *
     * @param host_ptr Puntero real del proceso que se desea localizar.
     * @return         ID de la arena si se encuentra, -1 en caso contrario.
     */
    int ArenaManager::find_arena_id_for_ptr(void *host_ptr) {
        for (const auto &[id, arena]: this->arenas) {
            if (arena.ptr == host_ptr) {
                return static_cast<int>(id); // cast para devolver como int
            }
        }
        return -1; // no encontrado
    }

} // namespace vm
