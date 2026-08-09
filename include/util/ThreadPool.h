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
 * @file ThreadPool.h
 * @brief Pool de hilos para ejecucion asincrona de tareas con soporte de futures.
 *
 * Implementacion minima, segura y sin dependencias externas.  En Linux usa futex
 * para notificar a los hilos worker sin overhead de condition_variable; en Windows
 * usa WaitOnAddress / WakeByAddressSingle.
 *
 * Uso tipico:
 * @code
 *   ThreadPool pool(4);
 *   auto fut = pool.submit([]{ return 42; });
 *   int result = fut.get();  // 42
 *   pool.shutdown();
 * @endcode
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <stdexcept>

#ifdef WIN32
#include "Windows.h"           // WakeByAddressSingle / WaitOnAddress
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

/**
 * @brief Suspende el hilo actual hasta que *addr != expected o se recibe una senal.
 *
 * Envuelve la llamada al sistema SYS_futex(FUTEX_WAIT) de Linux.
 *
 * @param addr     Puntero al contador atomico que se vigila.
 * @param expected Valor esperado del contador; si difiere, retorna inmediatamente.
 * @return Valor de retorno del syscall (0 en exito, -1 en error).
 */
static int futex_wait(std::atomic<int>* addr, int expected) {
    return syscall(SYS_futex,
                   reinterpret_cast<int*>(addr),
                   FUTEX_WAIT,
                   expected,
                   nullptr, nullptr, 0);
}

/**
 * @brief Despierta hasta @p count hilos suspendidos en la direccion @p addr.
 *
 * Envuelve la llamada al sistema SYS_futex(FUTEX_WAKE) de Linux.
 *
 * @param addr  Puntero al contador atomico donde estan esperando los hilos.
 * @param count Numero maximo de hilos a despertar.
 * @return Numero de hilos despertados.
 */
static int futex_wake(std::atomic<int>* addr, int count) {
    return syscall(SYS_futex,
                   reinterpret_cast<int*>(addr),
                   FUTEX_WAKE,
                   count,
                   nullptr, nullptr, 0);
}

#endif

/**
 * @brief Pool de hilos de proposito general con cola de tareas y futures.
 *
 * Crea N hilos worker al construirse y los mantiene esperando tareas.
 * Las tareas se encolan con enqueue() (sin resultado) o submit() (con future).
 * Al destruirse o llamar a shutdown() espera a que todas las tareas pendientes
 * terminen antes de unir los hilos.
 *
 * Sincronizacion:
 *   - Cola protegida por tasks_m_.
 *   - Notificacion de nuevas tareas mediante wake_flag_ + futex/WakeByAddressSingle.
 *   - Contador active_tasks_ para detectar cuando el pool esta inactivo.
 */
class ThreadPool {
public:
    /**
     * @brief Construye el pool con @p n hilos worker.
     *
     * Si @p n es 0 usa std::thread::hardware_concurrency(); si ese valor tambien
     * es 0 crea exactamente 1 hilo para garantizar funcionalidad minima.
     *
     * @param n Numero de hilos worker (0 = automatico).
     */
    explicit ThreadPool(size_t n = 0);

    /**
     * @brief Destructor: llama a shutdown() y espera a que todos los hilos terminen.
     */
    ~ThreadPool();

    /// No copiar ni mover: contiene hilos y mutexes
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    /**
     * @brief Encola una tarea y devuelve un future con el resultado.
     *
     * @tparam F    Tipo del callable.
     * @tparam Args Tipos de los argumentos del callable.
     * @param f    Callable a ejecutar de forma asincrona.
     * @param args Argumentos que se pasan al callable.
     * @return std::future<R> donde R = std::invoke_result_t<F, Args...>.
     * @throws std::runtime_error si el pool esta en proceso de apagado.
     */
    template<typename F, typename... Args>
    auto submit(F &&f, Args &&... args) -> std::future<typename std::invoke_result_t<F, Args...> >;

    /**
     * @brief Inicia el apagado del pool.
     *
     * Deja de aceptar nuevas tareas y espera a que las tareas pendientes terminen.
     * Tras esta llamada, enqueue() y submit() no tendran efecto.
     */
    void shutdown();

    /**
     * @brief Indica si el pool esta completamente inactivo.
     *
     * Devuelve true unicamente cuando:
     *   - No quedan tareas en la cola interna.
     *   - Ningun hilo worker esta ejecutando una tarea (active_tasks_ == 0).
     *
     * Util para detectar cuando todas las tareas enviadas han finalizado antes
     * de hacer shutdown().
     *
     * @note Thread-safe; no bloquea.
     * @return true si la cola esta vacia y ningun worker esta activo.
     */
    bool idle();

    /**
     * @brief Encola una tarea sin devolver un future.
     *
     * Inserta la tarea en la cola y despierta a un hilo worker.
     * Si el pool esta en proceso de apagado la tarea se descarta silenciosamente.
     *
     * @tparam F Tipo del callable (debe ser invocable sin argumentos).
     * @param f  Callable que representa la tarea.
     *
     * @warning Si el callable lanza una excepcion dentro del worker, esta se captura
     *          y se descarta (no se propaga al llamante).
     */
    template<typename F>
    void enqueue(F &&f) {
        {
            std::lock_guard lk(tasks_m_);
            if (stopping_.load()) return; // pool en apagado: descartar tarea
            tasks_.emplace(std::forward<F>(f));
        }

        wake_flag_.store(1, std::memory_order_release); // senalizar nueva tarea

#ifdef WIN32
        WakeByAddressSingle(&wake_flag_); // despertar un worker (Windows)
#elif defined(__linux__)
        futex_wake(&wake_flag_, 1);       // despertar un worker (Linux)
#else
        tasks_cv_.notify_one();           // despertar un worker (BSD/macOS)
#endif
    }

private:
    std::atomic<int> wake_flag_{0}; ///< Indicador de tarea disponible para futex/WaitOnAddress

    /**
     * @brief Bucle principal de cada hilo worker.
     *
     * Espera tareas en la cola, las ejecuta y repite hasta que stopping_ sea true
     * y la cola este vacia.
     */
    void worker_loop();

    std::vector<std::thread>           workers_; ///< Hilos worker del pool
    std::queue<std::function<void()> > tasks_;   ///< Cola de tareas pendientes
    std::mutex                         tasks_m_; ///< Mutex que protege tasks_
    std::condition_variable            tasks_cv_;///< No usado en la implementacion actual (reservado)
    std::atomic<bool>                  stopping_{false}; ///< true cuando el pool esta en apagado
};

// ---------------------------------------------------------------------------
// Implementacion inline de submit()
// ---------------------------------------------------------------------------

template<typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&... args) -> std::future<typename std::invoke_result_t<F, Args...> > {
    using R       = typename std::invoke_result_t<F, Args...>;
    // envolver el callable en un packaged_task para obtener el future
    auto task_ptr = std::make_shared<std::packaged_task<R()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<R> fut = task_ptr->get_future();

    {
        std::lock_guard<std::mutex> lk(tasks_m_);
        if (stopping_.load())
            throw std::runtime_error("ThreadPool is stopping, cannot submit new tasks");
        tasks_.emplace([task_ptr]() { (*task_ptr)(); }); // envolver en lambda sin argumentos
    }

    wake_flag_.store(1, std::memory_order_release); // notificar nueva tarea

#ifdef WIN32
    WakeByAddressSingle(&wake_flag_);
#elif defined(__linux__)
    futex_wake(&wake_flag_, 1);
#else
    tasks_cv_.notify_one();
#endif
    return fut;
}

#endif // THREADPOOL_H
