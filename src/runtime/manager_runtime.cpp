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
 * @file manager_runtime.cpp
 * @brief Implementacion del gestor de instancias de maquina virtual de VestaVM.
 *
 * Implementa @c ManagerRuntime: creacion, arranque y destruccion de instancias
 * VM, enrutamiento de comandos y gestion del ciclo de vida de los servidores TCP.
 */#include "runtime/manager_runtime.h"

#include <algorithm>

#include "runtime/runtime.h"
#include "distrib/dist_runtime.h"

namespace runtime {

    /**
     * @brief Construye el gestor de instancias VM.
     *
     * Asocia el listener TCP opcional y asigna el identificador del gestor.
     * El ArenaManager publico y el Loader publico se inicializan con sus
     * constructores por defecto.
     *
     * @param listener Puntero al listener TCP que acepta conexiones remotas
     *                 (puede ser nullptr si no se usa red).
     * @param id       Identificador unico del gestor dentro del sistema.
     */
    ManageVM::ManageVM(
        ManagerTCPListener *listener,
        uint64_t id
    ): id(id) {
        this->listener = listener; // asociar el listener TCP (puede ser nullptr)
    }

    /**
     * @brief Destructor: destruye todas las instancias VM gestionadas.
     *
     * Llama a destroy_all_vms() para liberar de forma ordenada todos los
     * recursos de las VMs antes de que el gestor sea destruido.
     */
    ManageVM::~ManageVM() {
        destroy_all_vms(); // liberar todas las instancias VM antes de destruir el gestor
    }

    /**
     * @brief Crea una nueva instancia VM con el numero de schedulers indicado.
     *
     * Incrementa el contador monotono para asignar un ID unico a la VM,
     * la construye en el heap con un unique_ptr y la almacena en el vector
     * de instancias.  La operacion esta protegida por vm_mutex.
     *
     * @param num_schedulers Numero de schedulers (hilos nativos) que la VM utilizara.
     * @return ID unico asignado a la nueva instancia VM.
     */
    uint64_t ManageVM::create_vm(size_t num_schedulers) {
        std::lock_guard lock(vm_mutex);         // proteger el acceso concurrente al vector de VMs
        uint64_t id = ++counter_vm;             // asignar ID monotonamente creciente

        // crear la VM en el heap; el unique_ptr gestiona su ciclo de vida
        vms.push_back(std::make_unique<VM>(*this, id, num_schedulers));
        return id; // devolver el ID asignado para que el llamador pueda referenciar la VM
    }

    /**
     * @brief Destruye la instancia VM identificada por @p vm_id.
     *
     * Busca la VM en el vector mediante busqueda lineal por ID y la elimina.
     * El unique_ptr libera automaticamente todos los recursos de la VM.
     * La operacion esta protegida por vm_mutex.
     *
     * @param vm_id ID de la VM a destruir.
     * @return true si la VM fue encontrada y destruida; false si no existe.
     */
    bool ManageVM::destroy_vm(uint64_t vm_id) {
        std::lock_guard lock(vm_mutex); // bloqueo exclusivo para modificar el vector

        // buscar la VM por ID usando un predicado lambda
        auto it = std::find_if(vms.begin(), vms.end(),
                               [vm_id](const std::unique_ptr<VM> &vm) {
                                   return vm->id == vm_id;
                               });

        if (it == vms.end())
            return false; // VM no encontrada

        vms.erase(it); // el unique_ptr libera automaticamente la VM al borrar el elemento
        return true;   // destruccion correcta
    }

    /**
     * @brief Devuelve un puntero a la instancia VM con el ID indicado.
     *
     * Realiza busqueda lineal en el vector protegido por vm_mutex.  La
     * direccion devuelta es estable mientras la VM no sea destruida.
     *
     * @param vm_id ID de la VM a localizar.
     * @return Puntero a la VM, o nullptr si no existe.
     */
    VM *ManageVM::get_vm(uint64_t vm_id) {
        std::lock_guard lock(vm_mutex); // bloqueo compartido de lectura
        for (auto& vm : vms) {
            if (vm->id == vm_id)
                return vm.get(); // puntero estable; la VM vive en el heap
        }
        return nullptr; // VM no encontrada
    }

    /**
     * @brief Comprueba si existe una instancia VM con el ID indicado.
     *
     * @param vm_id ID de la VM a buscar.
     * @return true si la VM existe; false en caso contrario.
     */
    bool ManageVM::has_vm(uint64_t vm_id) const {
        std::lock_guard lock(vm_mutex); // bloqueo de lectura
        for (auto& vm : vms)
            if (vm->id == vm_id)
                return true; // VM encontrada

        return false; // ninguna VM coincide con el ID
    }

    /**
     * @brief Devuelve el numero de instancias VM actualmente gestionadas.
     *
     * @return Numero de VMs en el vector de instancias.
     */
    size_t ManageVM::vm_count() const {
        std::lock_guard lock(vm_mutex); // bloqueo de lectura
        return vms.size();              // numero de instancias activas
    }

    /**
     * @brief Destruye todas las instancias VM y reinicia el contador de IDs.
     *
     * Limpia el vector de VMs (los unique_ptr liberan cada instancia) y
     * pone counter_vm a 0 para que el proximo create_vm() asigne el ID 1.
     * La operacion esta protegida por vm_mutex.
     */
    void ManageVM::destroy_all_vms() {
        std::lock_guard lock(vm_mutex); // bloqueo exclusivo para modificar el vector
        vms.clear();                    // unique_ptr libera automaticamente cada VM
        counter_vm = 0;                 // reiniciar el contador de IDs
    }

    /**
     * @brief Genera un resumen textual del estado del gestor de instancias.
     *
     * Incluye el nombre del gestor, el contador de VMs, el numero de instancias
     * activas, la direccion del listener TCP, del loader y del ArenaManager
     * publico, ademas de la tabla de instancias VM activas.
     *
     * @return Cadena con el resumen formateado del gestor.
     */
    std::string ManageVM::to_string_vm_manager_info() const {
        std::lock_guard lock(vm_mutex); // bloqueo de lectura para acceder a los campos
        std::ostringstream ss;

        ss << "=== ManageVM info ===\n";
        ss << "name_manager : " << name_manager << "\n";               // nombre identificativo del gestor
        ss << "counter_vm   : " << counter_vm   << "\n";               // ultimo ID asignado
        ss << "vm_count     : " << vms.size()   << "\n";               // numero de instancias activas

        // mostrar la direccion del listener TCP o "<null>" si no hay ninguno
        ss << "listener     : " << (listener
                                        ? ("ptr=" + std::to_string(reinterpret_cast<uintptr_t>(listener)))
                                        : std::string("<null>")) << "\n";

        ss << "loader       : ptr=" << reinterpret_cast<uintptr_t>(&loader)      << "\n"; // puntero al loader publico
        ss << "manager_mem  : ptr=" << reinterpret_cast<uintptr_t>(&manager_mem) << "\n"; // puntero al ArenaManager publico

        ss << "\nVM list:\n";
        print_vm_list_table(ss, vms); // delegar la impresion de la tabla en la funcion auxiliar
        return ss.str();
    }

    /**
     * @brief Imprime el resumen del gestor de instancias en la salida sincronizada.
     *
     * Delega en to_string_vm_manager_info() y escribe el resultado con vesta::scout()
     * para garantizar que la salida sea thread-safe.
     */
    void ManageVM::print_vm_manager_info() {
        vesta::scout() << to_string_vm_manager_info(); // escritura thread-safe en stdout
    }

} // namespace runtime
