/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/sampler_profiler.h
 * @brief Sampling profiler del codigo JIT (SIGPROF + setitimer).
 *
 * Muestrea periodica-mente el PC nativo del hilo de ejecucion y lo
 * clasifica en tres categorias:
 *   - Funcion Vex JIT-compilada (via el registro de regiones de @c auto_jit,
 *     @c jit::lookup_function_by_native_pc).
 *   - "interp"   : el hilo esta ejecutando bytecode en el interprete.
 *   - "runtime"  : overhead de la VM (compile JIT, scheduler, etc.).
 *
 * Diseno (importante):
 *   - El handler de SIGPROF hace SOLO trabajo async-signal-safe: lee el
 *     RIP del ucontext y lo encola en un ring lock-free de un solo slot
 *     por productor (reserve + publish).  NUNCA toca mutex ni el registro
 *     de regiones JIT: si el signal llegara mientras el hilo compilando
 *     mantiene @c g_regions_mtx, el handler se auto-bloquearia.
 *   - Un hilo consumidor drena el ring fuera del hot path, resuelve los
 *     nombres de funcion con locks normales y acumula el histograma.
 *
 * Al detenerse imprime el ranking (funcion, muestras, % del total) y,
 * si se pidio, escribe un archivo en formato "folded" de Brendan Gregg
 * (una linea por frame: "<funcion> <count>") listo para convertir a
 * flamegraph con @c tools/flamegraph.py o flamegraph.pl.
 *
 * Cero overhead cuando no se usa: el flag CLI @c --profile-samples solo
 * llama a @c sampler_profiler_start, que a su vez solo instala el
 * sigaction/SIGPROF + setitimer cuando se invoca explicitamente.
 */

#ifndef VESTA_JIT_SAMPLER_PROFILER_H
#define VESTA_JIT_SAMPLER_PROFILER_H

#include <cstdint>
#include <string>

namespace jit {

    /**
     * @brief Arranca el sampling profiler con periodo @p interval_ms.
     *
     * Instala un handler SIGPROF + ITIMER_PROF (solo plataformas POSIX
     * con @c setitimer/@c ucontext).  En Windows devuelve @c false sin
     * efecto.  Idempotente: una segunda llamada con el profiler activo
     * es no-op.
     *
     * @param interval_ms Periodo de muestreo en milisegundos (>=1; se
     *                    clampa a 1 si se pasa 0).
     * @param folded_path Ruta del archivo folded a escribir al parar
     *                    ("" = no escribir archivo).
     * @return true si el profiler quedo muestreando.
     */
    bool sampler_profiler_start(int interval_ms, const std::string &folded_path);

    /**
     * @brief Detiene el profiler, drena el ring y reporta.
     *
     * Desarma el timer, restaura el handler de SIGPROF previo, une el
     * hilo consumidor e imprime el ranking a stderr.  Si @p folded_path
     * fue no-vacio en @c sampler_profiler_start, escribe el archivo
     * folded.  Idempotente (no-op si el profiler no estaba activo).
     */
    void sampler_profiler_stop();

    /**
     * @brief True si el sampling profiler esta activo.
     */
    bool sampler_profiler_active();

} // namespace jit

#endif // VESTA_JIT_SAMPLER_PROFILER_H
