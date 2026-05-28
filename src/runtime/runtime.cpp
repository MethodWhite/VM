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
 * @file runtime.cpp
 * @brief Implementacion de la maquina virtual principal de VestaVM.
 *
 * Implementa @c VM: creacion del pool de hilos, gestion de schedulers,
 * arranque y parada del runtime, carga de ejecutables .velb y creacion
 * de procesos virtuales con @c make_process() / @c make_ready().
 */#include "runtime/runtime.h"

#include "cli/sync_io.h"
#include "runtime/manager_runtime.h"
#include "runtime/exception_runtime.h"
#include "distrib/dist_runtime.h"
#include <chrono>  // std::chrono::milliseconds para wait_for del shared_gc_cv

namespace runtime {

    /**
     * @brief Construye la instancia VM e inicializa todos sus schedulers.
     *
     * Conecta la VM al gestor de instancias @p mgr_vm_, asigna el ID indicado
     * y crea @p num_schedulers schedulers listos para lanzarse con start().
     *
     * @param mgr_vm_        Gestor de instancias que posee esta VM.
     * @param id_vm          Identificador unico de esta instancia dentro del gestor.
     * @param num_schedulers Numero de schedulers (hilos nativos) a crear.
     */
    VM::VM(
        ManageVM &mgr_vm_,
        uint64_t  id_vm,
        size_t    num_schedulers
    ) : pool(num_schedulers > 0 ? num_schedulers : 1), // right-sized: crear solo los threads que se usaran como schedulers (el default hardware_concurrency() = 8-16 desperdiciaba 8-16ms en spawn de threads inactivos en Windows)
        manager_mem_public(mgr_vm_.manager_mem),               // referencia al ArenaManager publico del gestor
        loader_priv(std::make_unique<loader::Loader>(mgr_vm_)), // cargador privado de esta instancia
        loader_public(mgr_vm_.loader),                          // referencia al cargador publico del gestor
        mgr_vm(mgr_vm_),
        num_schedulers(num_schedulers) {
        id = id_vm; // asignar el ID de la instancia

        schedulers.reserve(num_schedulers); // reservar capacidad para evitar realocaciones

        // crear cada scheduler y transferir su propiedad al vector
        for (uint32_t i = 0; i < num_schedulers; i++) {
            auto sched = std::make_unique<Scheduler>(i, *this); // scheduler con ID i
            schedulers.push_back(std::move(sched));             // mover al vector
        }

        // inicializar el runtime distribuido con configuracion minima:
        // permite IPC local (msgsend/msgrecv entre procesos de la misma VM)
        // sin abrir ningun puerto TCP ni hilo de descubrimiento UDP.
        // Para activar la red llamar a dist_runtime->start() explicitamente.
        // perf: el constructor de DistRuntime ya no inicializa
        // OpenSSL RNG (genera node_id solo cuando se llama start()), asi
        // que esto es esencialmente gratis (~us) para programas locales.
        distrib::DistRuntimeConfig dr_cfg{};
        dr_cfg.local_node_id    = 0;      // se genera lazy en start() si hace falta
        dr_cfg.vdp_listen_port  = 0;      // sin servidor TCP por defecto
        dr_cfg.discover_port    = 0;      // sin descubrimiento UDP
        dr_cfg.enable_discovery = false;
        std::snprintf(dr_cfg.local_node_name, sizeof(dr_cfg.local_node_name),
                      "vm-%llu", static_cast<unsigned long long>(id_vm));
        dist_runtime = std::make_unique<distrib::DistRuntime>(*this, dr_cfg);

        // registrar la clase predefinida @c FatalError en el
        // ClassRegistry del Loader publico (compartido por todas las VMs
        // del manager).  Idempotente: la primera VM crea la clase, las
        // siguientes la encuentran ya creada.  Sin esto, los opcodes que
        // intentaran lanzar @c FatalError caerian al camino antiguo
        // (mata el proceso sin posibilidad de captura).
        runtime::init_exception_classes(loader_public);

        // Instalar el handler de access violations a nivel de OS
        // (Vectored Exception Handler en Windows, sigaction(SIGSEGV) en
        // POSIX).  Idempotente via std::once: solo la primera VM
        // creada lo registra; las siguientes son no-op.  Permite que
        // accesos a punteros host invalidos desde bytecode (caso comun:
        // deref de host_ptr stale tras free) se conviertan en
        // FatalError capturables via try/catch en lugar de crashear la
        // VM entera.
        runtime::install_host_av_handler();
    }

    /**
     * @brief Genera una representacion textual de la instancia VM.
     * @return Cadena con el ID de la VM en hexadecimal.
     */
    std::string VM::to_string() const {
        return this->vm_summary(); // delegar en vm_summary para no duplicar logica
    }

    /**
     * @brief Imprime el estado de la VM en stdout usando la salida sincronizada.
     */
    void VM::print() {
        vesta::scout() << to_string(); // usar salida thread-safe
    }

    /**
     * @brief Lanza todos los schedulers en el ThreadPool y comienza la ejecucion.
     *
     * Para cada scheduler se somete una tarea al pool que ejecuta run_loop().
     * Los futures resultantes se almacenan en scheduler_futures para poder
     * esperar a su finalizacion en wait().
     *
     * @note Este metodo no bloquea; llamar a wait() para esperar la finalizacion.
     */
    void VM::start() {
        vm_running = true; // marcar la VM como activa

        // Optimizacion (single-scheduler bypass): cuando hay un solo
        // scheduler y el modo NO es persistente (dist-server), ejecutamos
        // el run_loop SINCRONAMENTE en el thread llamante (main).  Esto
        // ahorra:
        //   - Latencia de OS-thread sched (~30 ms en Windows) del worker
        //     del ThreadPool.
        //   - Sincronizacion via condition_variable (cv.wait posterior
        //     en el caller despierta inmediatamente porque run_loop
        //     setea vm_running=false antes de retornar).
        //
        // Para programas triviales y tooling esta latencia es muy notable
        // (representa el 50%+ del wall time).  Para programas grandes el
        // overhead del pool es despreciable, pero la mejora sigue siendo
        // 0% perdida y la simplicidad de single-thread es atractiva.
        //
        // El modo persistente (vm_persistent=true, dist-server) NO se
        // beneficia: el run_loop nunca termina por si solo (espera procesos
        // remotos via rspawn), bloquearia main para siempre.  Mantenemos
        // el ThreadPool para ese caso.
        if (num_schedulers == 1 && !vm_persistent) {
            schedulers[0]->run_loop(); // bloquea hasta que todos los procs mueran
            // run_loop ya setea vm_running=false + notifica done_cv cuando
            // has_alive_processes() retorna false.  Defensivo: asegurar
            // notify por si el run_loop salio por otra ruta (e.g. should_kill).
            {
                std::lock_guard<std::mutex> lk(done_mtx);
                vm_running = false;
            }
            done_cv.notify_all();
            return;
        }

        for (uint32_t i = 0; i < num_schedulers; i++) {
            // someter la tarea del scheduler al pool y guardar el future
            std::future<void> fut = pool.submit([this, i] {
                schedulers[i]->run_loop(); // ejecutar el bucle principal del scheduler
            });

            scheduler_futures.push_back(fut.share()); // almacenar como shared_future
        }
    }

    /**
     * @brief Solicita la parada cooperativa de todos los schedulers y espera a que terminen.
     *
     * Pone vm_running=false y should_kill=true en cada scheduler, luego despierta
     * a los que esten bloqueados en el semaforo.  Finalmente bloquea hasta que todos
     * los futures hayan completado y limpia el vector de futures.
     */
    void VM::stop() {
        vm_running = false; // indicar a todos los schedulers que la VM debe detenerse

        // marcar cada scheduler para que salga de su run_loop
        for (auto &sched: schedulers) {
            sched->should_kill = true; // bandera de parada cooperativa
        }

        // despertar a los schedulers bloqueados en sem.acquire()
        for (auto &sched: schedulers) {
            sched->sem.release(); // desbloquear el semaforo de espera
        }

        // esperar a que todos los hilos de los schedulers terminen
        for (auto &f: scheduler_futures) {
            f.get(); // bloquear hasta que el scheduler finalice
        }

        scheduler_futures.clear(); // limpiar futures consumidos
    }

    /**
     * @brief Devuelve un puntero al proceso con el PID global indicado.
     *
     * Usa pid.scheduler_id para localizar el scheduler correcto y luego
     * realiza la busqueda O(1) en su pid_index.
     *
     * @param pid PID global del proceso a buscar.
     * @return    Puntero al proceso, o nullptr si el PID no existe.
     */
    ProcessVM *VM::get_process(GlobalPID pid) {
        return schedulers[pid.scheduler_id]->pid_index[pid]; // busqueda directa en el indice
    }

    /**
     * @brief Bloquea el hilo llamante hasta que todos los schedulers finalicen.
     *
     * Llama a wait() sobre cada shared_future para sincronizar con el fin de
     * todos los run_loop() de los schedulers.
     */
    void VM::wait() {
        for (auto &f: scheduler_futures) {
            f.wait(); // bloquear hasta que cada scheduler termine su run_loop
        }
    }

    /**
     * @brief Indica si el ThreadPool de la VM esta inactivo (todos los schedulers han terminado).
     * @return true si el pool no tiene tareas activas ni pendientes.
     */
    bool VM::all_schedulers_dead() {
        bool is_dead = pool.idle(); // consultar el estado del pool
        return is_dead;
    }

    /**
     * @brief Indica si existe al menos un proceso vivo en algun scheduler.
     *
     * Itera los schedulers comprobando su alive_count atomico en O(N_schedulers),
     * evitando iterar todos los procesos de cada scheduler.
     *
     * @return true si alguno de los schedulers tiene alive_count > 0.
     */
    bool VM::has_alive_processes() {
        for (auto &sched: schedulers) {
            if (sched->alive_count > 0)
                return true; // al menos un proceso vivo en este scheduler
        }
        return false; // ningun scheduler tiene procesos vivos
    }

    /**
     * @brief Marca un proceso como READY para que su scheduler pueda ejecutarlo.
     *
     * Delega en make_ready() del scheduler propietario y llama a sem.release()
     * para despertar al scheduler si esta esperando.
     *
     * @note Tambien resetea should_kill del scheduler para que pueda aceptar
     *       nuevos procesos tras haber finalizado un ciclo completo.
     *
     * @param pid PID global del proceso que debe pasar a estado READY.
     */
    void VM::make_ready(GlobalPID pid) {
        schedulers[pid.scheduler_id]->make_ready(pid);  // insertar en la cola del scheduler
        schedulers[pid.scheduler_id]->sem.release();    // despertar al scheduler si duerme

        // reactivar el scheduler en caso de que haya terminado todos sus procesos
        // y should_kill se haya puesto a true; necesario para reutilizar el scheduler
        schedulers[pid.scheduler_id]->should_kill = false;
    }

    /**
     * @brief Crea un nuevo proceso virtual en el scheduler con menos carga (round-robin).
     *
     * Lanza una excepcion si no hay schedulers disponibles (start() no fue llamado).
     *
     * @return PID global del proceso recien creado.
     * @throws std::runtime_error si la VM no tiene schedulers activos.
     */
    GlobalPID VM::spawn_process() {
        if (schedulers.empty())
            throw std::runtime_error("VM::spawn_process: no hay gestores de procesos, posiblemente no llamo a start()");

        size_t index = next_sched;                            // indice del scheduler elegido
        next_sched   = (next_sched + 1) % schedulers.size(); // avanzar el contador round-robin

        return schedulers[index]->spawn(); // crear el proceso en el scheduler elegido
    }

    /**
     * @brief Reinicia completamente la VM conservando la estructura de schedulers.
     *
     * Detiene la VM con stop(), reinicia el estado interno de cada scheduler,
     * restaura los contadores y vuelve a lanzar los schedulers con start().
     */
    void VM::reset() {
        stop(); // detener la VM: vm_running=false, should_kill=true, esperar futures, limpiar futures

        // reiniciar el estado interno de cada scheduler
        for (auto &sched: schedulers) {
            sched->reset(); // limpia procesos, colas, pid_index, FSM, contadores y flags
        }

        // restaurar el estado de la instancia VM
        next_sched = 0;    // reiniciar el selector round-robin
        vm_running = true; // marcar como activa para que start() funcione

        scheduler_futures.clear(); // limpiar por si quedaron entradas residuales
        start(); // volver a lanzar los schedulers en el pool
    }

    // -------------------------------------------------------------------------
    // Recoleccion mayor del SharedHeap con STW coordinado.
    // -------------------------------------------------------------------------
    // El GC del SharedHeap requiere STW (stop-the-world) porque los handles
    // compartidos pueden ser referenciados desde stacks/registros de cualquier
    // proceso del VM.  Coordinacion:
    //  1. main thread setea @c shared_gc_active=true (libera los polls).
    //  2. cada scheduler, en su safepoint poll del run_loop, ve el flag y
    //     llama @c shared_gc_acks.fetch_add(1) + duerme en @c shared_gc_cv.
    //  3. main thread espera a que @c shared_gc_acks == num_schedulers.
    //  4. ejecuta el mark/sweep del @c shared_heap.
    //  5. setea @c shared_gc_active=false + notifica el cv.
    //  6. los schedulers despiertan y siguen el run_loop normal.
    uint32_t VM::shared_gc_collect() {
        // B3.3: mark + sweep STW del SharedHeap.
        //
        // Protocolo:
        //   1. Coordinacion STW: setea shared_gc_active=true + espera a
        //      que TODOS los schedulers ack en sus safe points.
        //   2. clear_marks(): zero del bitmap.
        //   3. Mark: scan stack + GP regs de cada ProcessVM buscando
        //      shared handles (value con bit 31 set y valido en
        //      shared_handle_table).  Marca el handle.
        //   4. Mark transitivo: BFS via fields del objeto (interpretando
        //      class_ptr para encontrar offsets GC).  Versionr v1:
        //      no transitivo - solo roots directos.  Suficiente para
        //      objetos shared simples sin referencias entre si.
        //   5. Sweep: para cada slot vivo no-marcado, free + unregister.
        //   6. Release STW.

        // Coordinacion STW (multi-scheduler).  Single-sched no la necesita.
        const uint32_t target_acks = static_cast<uint32_t>(schedulers.size());
        if (num_schedulers > 1) {
            shared_gc_acks.store(0, std::memory_order_release);
            shared_gc_active.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lk(shared_gc_mtx);
            shared_gc_cv.wait_for(lk, std::chrono::milliseconds(5000),
                [&]{ return shared_gc_acks.load(std::memory_order_acquire) >= target_acks; });
        }

        // Mark + sweep.
        shared_handle_table.clear_marks();

        // Mark phase: scan stack + regs de cada ProcessVM activo.  Para
        // cada qword, si es un GcHandle con bit 31 (shared) y resuelve a
        // un host_ptr valido en shared_handle_table, marcamos el slot.
        auto try_mark = [&](uint64_t v) {
            // Filter: rapido descarte de no-handles.
            if (v == 0) return;
            // Caso 1: el valor ES un handle directo.
            if ((v & 0xFFFFFFFF00000000ULL) == 0 || v <= 0xFFFFFFFFULL) {
                uint32_t h = static_cast<uint32_t>(v);
                if (h & gc::SHARED_HANDLE_BIT) {
                    if (shared_handle_table.lookup(h) != nullptr) {
                        shared_handle_table.mark(h);
                        return;
                    }
                }
            }
            // Caso 2: el valor es un host_ptr al payload (registrado).
            // O(N) lineal: aceptable como fallback.  Mejor: agregar
            // ptr_to_handle map en SharedHandleTable.  Para v1, skip.
        };

        for (auto &sched : schedulers) {
            if (!sched) continue;
            for (auto &p : sched->processes) {
                if (!p) continue;
                // GP regs R0..R15.
                for (int i = 0; i < 16; ++i) {
                    try_mark(p->registers.regs[i].raw());
                }
                // Stack scan [rsp, stack_high).  Bounded.
                uint64_t rsp = p->registers.stack_pointer.raw();
                uint64_t shi = p->stack_high;
                if (shi > rsp && (shi - rsp) < (16ull * 1024 * 1024)) {
                    for (uint64_t addr = rsp; addr + 8 <= shi; addr += 8) {
                        try_mark(p->vm_mem.read_u64(addr));
                    }
                }
            }
        }

        // Sweep phase: barrer slots no marcados.  El callback recibe
        // (host_ptr, size_bytes) para que SharedHeap libere el slot.
        uint32_t swept = shared_handle_table.sweep_unmarked(
            [this](uint8_t *p, uint32_t /*sz*/) {
                shared_heap.free(p);
            });

        // Release STW.
        if (num_schedulers > 1) {
            std::lock_guard<std::mutex> lk(shared_gc_mtx);
            shared_gc_active.store(false, std::memory_order_release);
            shared_gc_cv.notify_all();
        }
        return swept;
    }

} // namespace runtime
