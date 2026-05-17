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
 * @file runtime.h
 * @brief Declaracion de la maquina virtual principal de VestaVM.
 *
 * Declara @c VM: punto de entrada del runtime; gestiona el pool de hilos,
 * los schedulers, la memoria compartida y el ciclo de vida de los
 * procesos virtuales (carga, arranque, parada).
 */#ifndef RUNTIME_H
#define RUNTIME_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

#include "runtime/vm_state_event.h"
#include "decode_instruction.h"
#include "proceso_runtime.h"
#include "scheduler.h"
#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
#include "runtime/rflags.h"
#include "util/ThreadPool.h"

#include "runtime/pid.h"
#include "loader/oop_types.h"  // FutureObject + FutureState para shared_futures
#include <mutex>                // mutex para shared_futures concurrent access
#include <condition_variable>   // condition_variable para done_cv (rev3)

/** @brief Version actual del formato de bytecode .velb soportado por esta VM. */
#define VERSION_VM 0

namespace loader {
    class Loader; ///< Cargador de archivos .velb (declaracion adelantada)
} // namespace runtime
namespace debug { class Debugger; } ///< Servidor de debug TCP (declaracion adelantada)
namespace runtime {
}

namespace distrib {
    class DistRuntime; ///< Coordinador del sistema distribuido (declaracion adelantada)
}

namespace runtime {
    class ManageVM;  ///< Gestor de multiples instancias VM (declaracion adelantada)
    class VM;        ///< Instancia de la maquina virtual (declaracion adelantada)
    class Scheduler; ///< Planificador de procesos (declaracion adelantada)
    class ProcessVM; ///< Proceso virtual (declaracion adelantada)

    /**
     * @class VM
     * @brief Instancia de la maquina virtual VestaVM.
     *
     * Cada objeto VM gestiona un conjunto de schedulers (hilos nativos) que
     * ejecutan procesos virtuales en paralelo.  La VM no es el proceso: es el
     * contenedor que coordina N schedulers a traves de un ThreadPool.
     *
     * Ciclo de vida tipico:
     *   1. VM vm(mgr, id, n_schedulers) -- construir la instancia.
     *   2. vm.start()                   -- lanzar los schedulers (no bloquea).
     *   3. vm.spawn_process()           -- crear un proceso virtual.
     *   4. vm.make_ready(pid)           -- poner el proceso en estado READY.
     *   5. vm.wait()                    -- bloquear hasta que los schedulers terminen.
     *   6. vm.stop()                    -- senyal de parada cooperativa.
     */
    class VM {
    public:
        ThreadPool pool; ///< Pool de hilos que ejecuta los schedulers (dimensionado a num_schedulers, no hardware_concurrency, para no crear threads ociosos)

        /**
         * @brief Indice del proximo scheduler al que se asignara un proceso nuevo.
         *
         * Se incrementa en modulo num_schedulers para distribuir procesos de forma
         * round-robin entre los schedulers disponibles.
         */
        size_t next_sched = 0;

        std::vector<std::unique_ptr<Scheduler>> schedulers; ///< Schedulers de esta instancia (uno por hilo nativo)

        /**
         * @brief Futures de los schedulers generados en start().
         *
         * Cada future representa la tarea del ThreadPool asociada a un scheduler.
         * Se usan en wait() para bloquear hasta que todos terminen.
         */
        std::vector<std::shared_future<void>> scheduler_futures;

        vm::ArenaManager &manager_mem_public; ///< Gestor de memoria publica compartido con el ManageVM

        std::unique_ptr<loader::Loader> loader_priv; ///< Cargador privado de esta instancia VM

        loader::Loader &loader_public; ///< Cargador publico del ManageVM que contiene esta instancia

        uint64_t id; ///< Identificador unico de esta instancia VM dentro del ManageVM

        ManageVM &mgr_vm; ///< Referencia al gestor de instancias que posee esta VM

        // ---------------------------------------------------------------------
        // tabla compartida de FutureObjects para IPC entre procesos.
        // ---------------------------------------------------------------------
        // Los FutureObject viven aqui en lugar de en el gc_heap per-process,
        // porque el patron @c spawn { fulfill(...) } + parent.await(...) requiere
        // que ambos procesos accedan al mismo Future objeto.  Usar la tabla
        // compartida garantiza que el handle devuelto por @c future sea valido
        // desde cualquier proceso de la misma VM.
        //
        // Indices son monotonicos (no se reciclan) para evitar A-B-A; en la
        // practica un Future se libera mucho despues del fulfill y la tabla
        // crece pero no de forma critica (futures son pocos comparados con
        // objetos GC normales).  Mejora futura: free list + reciclaje.
        std::vector<loader::FutureObject> shared_futures;
        std::mutex                        shared_futures_mtx;

        // ---------------------------------------------------------------------
        // Argumentos de linea de comandos del programa Vex (argv).
        // ---------------------------------------------------------------------
        // Se pueblan desde main.cpp cuando se invoca `vm --run prog.velb arg1 arg2 ...`.
        // El programa los consulta via los builtins Vex `args_count()` y
        // `args_get(i)`, que bajan a los opcodes bytecode `getargc` (0x6B) y
        // `getarg` (0x6C) respectivamente.  El opcode `getarg` aloca un
        // StringObject GC-managed con el contenido del arg solicitado.
        std::vector<std::string> script_args;

        // ---------------------------------------------------------------------
        // condition variable para que el hilo principal
        // se bloquee sin polling hasta que la VM termine.
        // ---------------------------------------------------------------------
        // Antes el bucle en main era `while (vm.has_alive_processes())
        // sleep_for(1ms)` -> en Windows el granularity de Sleep es ~15.6ms,
        // anadiendo 15-50 ms de latencia por programa incluso para tareas
        // triviales.  Ahora el ultimo scheduler que detecte
        // `vm_running == false` notifica @c done_cv y main hace
        // `done_cv.wait` con predicado.  Cero polling, latencia ~us.
        std::mutex              done_mtx;
        std::condition_variable done_cv;

        /**
         * @brief Construye la instancia VM e inicializa los schedulers.
         *
         * Crea @p num_schedulers schedulers y los deja listos para lanzar con start().
         *
         * @param mgr_vm         Gestor de instancias que posee esta VM.
         * @param id_vm          Identificador unico de esta instancia.
         * @param num_schedulers Numero de schedulers (hilos nativos) a crear.
         */
        VM(ManageVM &mgr_vm, uint64_t id_vm, size_t num_schedulers);

        /**
         * @brief Genera una representacion textual de la instancia VM.
         * @return Cadena con el ID y el estado de los schedulers.
         */
        [[nodiscard]] std::string to_string() const;

        /**
         * @brief Imprime el estado completo de la VM en stdout (uso en depuracion).
         */
        void print();

        /**
         * @brief Lanza todos los schedulers y comienza la ejecucion de procesos.
         *
         * Este metodo NO bloquea.  Para esperar a que la VM finalice debe
         * llamarse a wait() despues.
         */
        void start();

        /**
         * @brief Solicita la parada cooperativa de todos los schedulers.
         *
         * Pone should_kill=true en cada scheduler y despierta a los que esten
         * bloqueados en el semaforo de espera.
         */
        void stop();

        /**
         * @brief Devuelve un puntero al proceso con el PID global indicado.
         *
         * Localiza el scheduler propietario usando pid.scheduler_id y delega en
         * el en la busqueda del proceso local.  No verifica la validez del PID.
         *
         * @param pid PID global del proceso a buscar.
         * @return    Puntero al proceso, o nullptr si no existe.
         */
        ProcessVM *get_process(GlobalPID pid);

        /**
         * @brief Bloquea el hilo llamante hasta que todos los schedulers finalicen.
         *
         * Equivale a join() sobre el conjunto de scheduler_futures.
         * La VM puede finalizar porque todos sus procesos terminaron o porque
         * se llamo a stop().
         */
        void wait();

        /**
         * @brief Indica si todos los schedulers han terminado su ejecucion.
         *
         * Consulta el estado idle del ThreadPool.  Un pool idle implica que:
         *   - Todos los schedulers salieron de run_loop().
         *   - No hay procesos en ejecucion dentro de la VM.
         *   - No hay tareas pendientes en el pool.
         *
         * @note No bloquea; inspecciona el estado actual del pool.
         * @return true si no queda ningun scheduler activo ni tareas pendientes.
         */
        bool all_schedulers_dead();

        /**
         * @brief Indica si existe al menos un proceso vivo en algun scheduler.
         * @return true si algun proceso esta en un estado distinto a DEAD o HALT.
         */
        bool has_alive_processes();

        /**
         * @brief Marca un proceso como READY para que el scheduler pueda ejecutarlo.
         *
         * Inserta el PID en la cola ready_queue del scheduler propietario.
         * Puede llamarse desde cualquier hilo (es thread-safe).
         *
         * @param pid PID global del proceso que debe pasar a estado READY.
         */
        void make_ready(GlobalPID pid);

        /**
         * @brief Crea un nuevo proceso virtual en el scheduler con menos carga.
         *
         * Elige el scheduler segun next_sched (round-robin) y llama a spawn()
         * en ese scheduler.
         *
         * @return PID global del proceso recien creado.
         */
        GlobalPID spawn_process();

        /**
         * @brief Reinicia el estado interno de la VM sin destruirla.
         */
        void reset();

        /**
         * @brief Genera un resumen compacto de la instancia VM.
         * @return Cadena con el ID en hexadecimal.
         */
        std::string vm_summary() const {
            std::ostringstream ss;
            ss << "ID=" << vesta::hex64((uint64_t) id) << "\n"; // mostrar ID en hexadecimal
            return ss.str();
        }

        std::atomic<bool> vm_running{true};      ///< true mientras haya schedulers en ejecucion
        std::atomic<bool> vm_persistent{false};  ///< si true, el scheduler espera nuevos procesos en lugar de terminar al quedarse sin trabajo

        size_t num_schedulers; ///< Numero de schedulers creados al inicializar la VM

        std::unique_ptr<distrib::DistRuntime> dist_runtime; ///< Coordinador del sistema de programacion distribuida (puede ser nullptr si no se arranco)

        // Servidor de debug TCP opcional.  Si es nullptr, el coste runtime
        // del path de depuracion es exactamente cero (los schedulers tienen
        // has_hooks=false y el fast path no llama a on_before_exec).
        // Cuando esta != nullptr, los schedulers ejecutan el slow path que
        // invoca debugger->on_before_exec() antes de cada instruccion;
        // dentro de on_before_exec, dos atomic loads (any_bp_, any_step_)
        // + 1 branch resuelven el caso "sin breakpoints ni step" en ~1 ns.
        // El puntero NO es propietario: el dueno es el caller (main.cpp via
        // unique_ptr local que vive durante toda la ejecucion).
        debug::Debugger *debugger = nullptr;

    private:
        std::mutex state_lock; ///< Mutex para serializar cambios de estado de la instancia
    };


    /**
     * @brief Vuelca en stdout el contenido de una region de memoria virtual.
     *
     * Itera las paginas del rango [@p vaddr, @p vaddr + @p size) resolviendo
     * cada una a traves del TLB e imprimiendo su direccion de host y los primeros
     * bytes en formato hexdump.  Las paginas no mapeadas se marcan como "not mapped".
     *
     * @param tlb   TLB del proceso cuya memoria se quiere volcar.
     * @param vaddr Direccion virtual de inicio del rango.
     * @param size  Tamanyo en bytes del rango a volcar.
     */
    static void dump_vm_region(tlb::LazyHybridTLB *tlb, uint64_t vaddr, size_t size) {
        uint64_t start = vaddr & ~0xFFFULL;                    // alinear inicio a limite de pagina
        uint64_t end   = (vaddr + size + 0xFFFULL) & ~0xFFFULL; // alinear fin al siguiente limite de pagina

        vesta::scout() << "==================== VM Memory Dump ====================\n";
        vesta::scout() << "Virtual region: 0x" << std::hex << start
                << " - 0x" << end
                << "  (" << std::dec << size << " bytes)\n\n";

        for (uint64_t page = start; page < end; page += 0x1000) {
            void *host = tlb->get_real_host_ptr_of_vptr(page); // resolver pagina a puntero de host

            if (!host) {
                vesta::scout() << "  [0x" << std::hex << page << "]  ->  <not mapped>\n"; // pagina no mapeada
                continue;
            }

            vesta::scout() << "  [0x" << std::hex << page << "]  ->  host=" << host << "\n"; // mostrar puntero de host

            const auto *p = static_cast<uint8_t *>(host); // puntero de lectura al inicio de la pagina

            vesta::scout() << "       data: ";
            vesta::scout() << vesta::dump(p, size); // volcar bytes en formato hexdump
            vesta::scout() << "\n";
        }

        vesta::scout() << "================== End VM Memory Dump ==================\n";
    }

} // namespace runtime

#endif // RUNTIME_H
