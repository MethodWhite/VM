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
 * @file ThreadPool.cpp
 * @brief Implementacion del pool de hilos de VestaVM.
 *
 * Implementa el constructor, destructor, shutdown(), idle() y worker_loop()
 * de la clase @c ThreadPool.  La notificacion de tareas usa futex en Linux
 * y WaitOnAddress/WakeByAddressAll en Windows.
 */
#include "util/ThreadPool.h"

#include <iostream>
#include <utility>

ThreadPool::ThreadPool(size_t n) {
    /* si n==0 usar todos los nucleos disponibles; garantizar al menos 1 */
    if (n == 0) {
        n = std::max<size_t>(1, std::thread::hardware_concurrency());
    }
    workers_.reserve(n);
    /* lanzar cada hilo worker apuntando a worker_loop() */
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] {
            this->worker_loop();
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown(); /* espera que todos los workers terminen antes de destruir */
}

void ThreadPool::shutdown() {
    {
        std::lock_guard lk(tasks_m_);
        stopping_.store(true); /* señal de parada a todos los workers */
    }
    /* despertar a todos los workers para que noten stopping_==true */
    wake_flag_.store(1, std::memory_order_release);

#ifdef WIN32
    WakeByAddressAll(&wake_flag_);      /* Windows: despertar todos */
#elif defined(__linux__)
    futex_wake(&wake_flag_, workers_.size()); /* Linux: despertar N hilos */
#else
    tasks_cv_.notify_all();             /* BSD/macOS: despertar todos */
#endif

    /* esperar a que cada hilo termine su iteracion actual */
    for (auto &t : workers_) {
        if (t.joinable()) t.join();
    }
}

bool ThreadPool::idle() {
    std::lock_guard lk(tasks_m_);
    return tasks_.empty(); /* true solo si no hay tareas pendientes en cola */
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        /* bucle de espera: bloquear hasta que haya tarea o se deba parar */
        while (true) {
            int expected = 0;

            /* comprobar cola SIN dormir, con mutex */
            {
                std::lock_guard lk(tasks_m_);
                if (!tasks_.empty()) break;    /* hay tarea: salir del bucle interno */
                if (stopping_.load()) return;  /* pool parando: terminar el hilo */
            }

            /* resetear flag antes de dormir para no perder señales */
            wake_flag_.store(0, std::memory_order_relaxed);

#ifdef WIN32
            WaitOnAddress(&wake_flag_, &expected, sizeof(int), INFINITE);
#elif defined(__linux__)
            futex_wait(&wake_flag_, expected);
#else
            /* BSD/macOS: condition_variable con predicado.  La cola y
             * stopping_ se revisan bajo el mutex, asi que el wait no pierde
             * notificaciones: notify_one/notify_all se emiten desde enqueue,
             * submit y shutdown con tasks_m_ protegido o tras liberarlo. */
            {
                std::unique_lock<std::mutex> lk(tasks_m_);
                tasks_cv_.wait(lk, [this] {
                    return !tasks_.empty() || stopping_.load();
                });
            }
#endif
        }

        /* extraer la tarea del frente de la cola */
        {
            std::lock_guard lk(tasks_m_);
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }

        if (task) task(); /* ejecutar la tarea; las excepciones se capturan si usa packaged_task */
    }
}
