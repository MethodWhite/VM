/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file src/jit/sampler_profiler.cpp
 * @brief Implementacion del sampling profiler JIT (ver sampler_profiler.h).
 *
 * Solo se compila el codigo POSIX (setitimer + SIGPROF + ucontext); en
 * Windows @c sampler_profiler_start devuelve false y nada mas se enlaza.
 *
 * El ring lock-free:
 *   - @c g_reserved : contador "reserve" (cada handler reserva un slot
 *     unico via fetch_add).  Los datos se escriben DESPUES de reservar
 *     pero ANTES de publicar.
 *   - @c g_published : contador "publish" con memory_order_release; el
 *     consumidor hace load(acquire) antes de leer un slot, asi la
 *     escritura del slot es visible (sincronizacion release/acquire).
 *   - Si el productor se adelanta mas de @c kRingSize slots sobre el
 *     consumidor, se descarta la muestra (o sobreescribe la vieja, que
 *     el consumidor ya descarto): para un profiler es aceptable.
 */

#include "jit/sampler_profiler.h"

#include "jit/auto_jit.h"                 // lookup_function_by_native_pc
#include "runtime/exception_runtime.h"    // get_current_executing_process (TLS)

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/time.h>
    #include <ucontext.h>
    #define VESTA_SAMPLER_POSIX 1
#endif

namespace jit {

    namespace {

        /* ------------------------------------------------------------------ */
        /* Estado compartido del sampler                                        */
        /* ------------------------------------------------------------------ */

        constexpr size_t   kRingSize = 1u << 16;  ///< 65536 slots (~1 MB).
        constexpr uint64_t kRingMask = kRingSize - 1;

        /// Una muestra cruda tomada por el handler.
        struct Sample {
            uint64_t rip      = 0;  ///< PC nativo interrumpido.
            uint8_t  has_proc = 0;  ///< 1 si habia un ProcessVM ejecutandose.
        };

        std::atomic<uint64_t> g_reserved{0};   ///< slots reservados (handler).
        std::atomic<uint64_t> g_published{0};  ///< slots listos para leer.
        std::atomic<uint64_t> g_consumed{0};   ///< slots consumidos (hilo).
        std::atomic<bool>     g_active{false}; ///< sampler encendido.

        Sample g_ring[kRingSize];              ///< escritos por el handler.

        std::thread g_consumer;                ///< hilo drenador.
        std::mutex  g_hist_mtx;                ///< protege g_hist + g_total.
        std::unordered_map<std::string, uint64_t> g_hist;
        uint64_t g_total = 0;                  ///< muestras totales.

        std::string g_folded_path;             ///< archivo folded destino.
        int         g_interval_ms = 10;        ///< periodo de muestreo (reporte).
        struct sigaction g_old_sigprof{};      ///< handler SIGPROF previo.
        bool g_had_old_sigprof = false;

        /* ------------------------------------------------------------------ */
        /* Productor (async-signal-safe)                                        */
        /* ------------------------------------------------------------------ */

        /**
         * @brief Encola una muestra cruda en el ring.  Solo reserva el slot,
         *        escribe los datos y publica -- sin locks ni malloc.
         */
        void record_sample(uint64_t rip, bool has_proc) noexcept {
            const uint64_t idx = g_reserved.fetch_add(1, std::memory_order_relaxed);
            const uint64_t slot = idx & kRingMask;
            g_ring[slot] = Sample{rip, has_proc ? uint8_t{1} : uint8_t{0}};
            /* Publicar DESPUES de escribir el slot: el consumidor que vea
             * g_published > slot via acquire vera los datos (release/acquire). */
            g_published.fetch_add(1, std::memory_order_release);
        }

#if defined(VESTA_SAMPLER_POSIX)
        void sigprof_handler(int /*sig*/, siginfo_t */*info*/, void *ctx) noexcept {
            if (!g_active.load(std::memory_order_relaxed)) return;
            uint64_t rip = 0;
            const ucontext_t *uc = static_cast<const ucontext_t *>(ctx);
            if (uc != nullptr) {
#if defined(__x86_64__)
                rip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
                rip = static_cast<uint64_t>(uc->uc_mcontext.pc);
#endif
            }
            /* TLS del hilo interrumpido: leerlo aqui es seguro (una simple
             * lectura de thread_local).  No-nulo -> ejecutando bytecode. */
            const bool has_proc = runtime::get_current_executing_process() != nullptr;
            record_sample(rip, has_proc);
        }
#endif

        /* ------------------------------------------------------------------ */
        /* Consumidor (fuera del hot path)                                      */
        /* ------------------------------------------------------------------ */

        /**
         * @brief Consume las muestras publicadas y las clasifica.
         *
         * Clasificacion:
         *   - El RIP cae en una region JIT registrada -> nombre de la funcion.
         *   - Si no, pero habia un proceso ejecutandose -> "interp".
         *   - Si no -> "runtime" (overhead de la VM: compile, scheduler, etc.).
         */
        void drain() {
            const uint64_t pub = g_published.load(std::memory_order_acquire);
            uint64_t cons = g_consumed.load(std::memory_order_relaxed);
            /* Si el productor se adelanto mas de kRingSize, los slots mas
             * viejos ya fueron sobreescritos: saltar hasta no dejar avanzar
             * mas que el tamano del ring (evita leer slots corruptos). */
            if (pub > cons + kRingSize) {
                cons = pub - kRingSize;
            }
            while (cons < pub) {
                const Sample s = g_ring[cons & kRingMask];
                ++cons;

                const std::string fn = jit::lookup_function_by_native_pc(s.rip);
                std::string key;
                if (!fn.empty()) {
                    key = std::move(fn);
                } else if (s.has_proc != 0) {
                    key = "interp";
                } else {
                    key = "runtime";
                }

                std::lock_guard<std::mutex> lk(g_hist_mtx);
                ++g_hist[key];
                ++g_total;
            }
            g_consumed.store(cons, std::memory_order_relaxed);
        }

        void consumer_loop() {
            while (g_active.load(std::memory_order_acquire)) {
                drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            drain();  /* drenar lo que quede al parar. */
        }

        /* ------------------------------------------------------------------ */
        /* Reporte                                                              */
        /* ------------------------------------------------------------------ */

        void report_and_write() {
            std::vector<std::pair<std::string, uint64_t>> items;
            {
                std::lock_guard<std::mutex> lk(g_hist_mtx);
                items.reserve(g_hist.size());
                for (const auto &kv : g_hist) {
                    items.emplace_back(kv.first, kv.second);
                }
            }
            std::sort(items.begin(), items.end(),
                      [](const auto &a, const auto &b) { return a.second > b.second; });

            std::fprintf(stderr, "\n=== SAMPLING PROFILER ===\n");
            std::fprintf(stderr, "muestras: %llu  (~%.2f s de CPU muestreados "
                                 "cada %d ms)\n",
                         static_cast<unsigned long long>(g_total),
                         static_cast<double>(g_total) * g_interval_ms / 1000.0,
                         g_interval_ms);
            for (const auto &item : items) {
                const double pct = g_total > 0
                    ? (100.0 * static_cast<double>(item.second) /
                       static_cast<double>(g_total))
                    : 0.0;
                std::fprintf(stderr, "  %6.2f%%  %10llu  %s\n",
                             pct, static_cast<unsigned long long>(item.second),
                             item.first.c_str());
            }
            std::fflush(stderr);

            if (!g_folded_path.empty()) {
                std::ofstream out(g_folded_path);
                if (out) {
                    for (const auto &item : items) {
                        out << item.first << ' ' << item.second << '\n';
                    }
                    out.flush();
                    std::fprintf(stderr,
                                 "[sampler] folded escrito en '%s'\n",
                                 g_folded_path.c_str());
                } else {
                    std::fprintf(stderr,
                                 "[sampler] error: no se pudo escribir '%s'\n",
                                 g_folded_path.c_str());
                }
            }
        }

    } // namespace

    /* ---------------------------------------------------------------------- */
    /* API publica                                                              */
    /* ---------------------------------------------------------------------- */

    bool sampler_profiler_start(int interval_ms, const std::string &folded_path) {
        if (g_active.load(std::memory_order_relaxed)) {
            return true;  /* idempotente */
        }
#if !defined(VESTA_SAMPLER_POSIX)
        (void)interval_ms;
        (void)folded_path;
        std::fprintf(stderr,
                     "[sampler] no soportado en esta plataforma "
                     "(requiere POSIX setitimer/SIGPROF)\n");
        return false;
#else
        if (interval_ms < 1) interval_ms = 1;
        g_folded_path = folded_path;
        g_interval_ms = interval_ms;

        struct sigaction sa{};
        sa.sa_flags     = SA_SIGINFO | SA_RESTART;
        sa.sa_sigaction = &sigprof_handler;
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, SIGPROF);  /* evitar reentrada del handler. */
        if (sigaction(SIGPROF, &sa, &g_old_sigprof) != 0) {
            g_had_old_sigprof = false;
            std::fprintf(stderr, "[sampler] error: sigaction(SIGPROF) fallo\n");
            return false;
        }
        g_had_old_sigprof = true;

        struct itimerval it{};
        it.it_interval.tv_sec  = interval_ms / 1000;
        it.it_interval.tv_usec = (interval_ms % 1000) * 1000;
        it.it_value            = it.it_interval;
        if (setitimer(ITIMER_PROF, &it, nullptr) != 0) {
            if (g_had_old_sigprof) sigaction(SIGPROF, &g_old_sigprof, nullptr);
            g_had_old_sigprof = false;
            std::fprintf(stderr, "[sampler] error: setitimer(ITIMER_PROF) fallo\n");
            return false;
        }

        g_active.store(true, std::memory_order_release);
        try {
            g_consumer = std::thread(&consumer_loop);
        } catch (const std::exception &) {
            /* Raro (falta de recursos de hilo): desarmar todo y avisar. */
            g_active.store(false, std::memory_order_release);
            struct itimerval zero{};
            (void)setitimer(ITIMER_PROF, &zero, nullptr);
            if (g_had_old_sigprof) {
                (void)sigaction(SIGPROF, &g_old_sigprof, nullptr);
                g_had_old_sigprof = false;
            }
            std::fprintf(stderr, "[sampler] error: no se pudo crear el hilo "
                                 "consumidor\n");
            return false;
        }
        return true;
#endif
    }

    void sampler_profiler_stop() {
#if defined(VESTA_SAMPLER_POSIX)
        if (!g_active.load(std::memory_order_relaxed)) {
            return;  /* idempotente */
        }
        g_active.store(false, std::memory_order_release);

        struct itimerval zero{};
        (void)setitimer(ITIMER_PROF, &zero, nullptr);  /* desarmar. */

        if (g_had_old_sigprof) {
            (void)sigaction(SIGPROF, &g_old_sigprof, nullptr);
            g_had_old_sigprof = false;
        }

        if (g_consumer.joinable()) {
            g_consumer.join();
        }
        drain();  /* por si quedo algo entre el exit del hilo y el join. */

        report_and_write();

        {
            std::lock_guard<std::mutex> lk(g_hist_mtx);
            g_hist.clear();
            g_total = 0;
        }
        g_reserved.store(0, std::memory_order_relaxed);
        g_published.store(0, std::memory_order_relaxed);
        g_consumed.store(0, std::memory_order_relaxed);
#endif
    }

    bool sampler_profiler_active() {
        return g_active.load(std::memory_order_relaxed);
    }

} // namespace jit
