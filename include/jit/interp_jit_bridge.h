/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/interp_jit_bridge.h
 * @brief Trampolines para cruzar la frontera interprete <-> codigo nativo
 *        JIT-eado.
 *
 * = Diseno =
 *
 * El JIT-eado de Vesta usa convencion VM: @c ProcessVM* en el primer
 * argumento (rdi/rcx segun host), args reales del bytecode en los
 * registros virtuales @c proc->registers.regs[1..N], return en
 * @c proc->registers.regs[0].
 *
 * Cuando el interprete decide saltar a una funcion JIT-eada:
 *
 *   1. @c enter_jit(fn, proc) toma el puntero al codigo nativo y el
 *      ProcessVM.  Llama la funcion via @c reinterpret_cast.
 *   2. La funcion JIT lee/escribe @c proc->registers segun necesite.
 *   3. Al @c ret, control vuelve a @c enter_jit que retorna al
 *      interprete.
 *
 * Si codigo JIT necesita VOLVER al interprete (e.g. para ejecutar un
 * opcode no soportado por C1, o para ejercer deopt), llama a un
 * trampoline @c jit_to_interp_callback que ajusta el bytecode_pc y
 * retorna del JIT con un flag especial.
 *
 * Por ahora @c return_from_jit es un stub: el flujo normal es que la
 * funcion JIT haga su trabajo, escriba el return en @c regs[0] y
 * haga @c RET.  Si la funcion JIT quiere salir antes (e.g. excepcion
 * o tail call al interprete), se hara via runtime entries que setean
 * el estado del ProcessVM.
 */

#ifndef VESTA_JIT_INTERP_JIT_BRIDGE_H
#define VESTA_JIT_INTERP_JIT_BRIDGE_H

#include <cstdint>

#include "vesta_rt/public.h"

namespace jit {

    /**
     * @brief Firma de funcion JIT-eada.  Convencion VM:
     *        - rdi/rcx: @c ProcessVM*
     *        - Args reales en @c proc->registers.regs[1..N]
     *        - Return en @c proc->registers.regs[0]
     *
     * El tipo retornado @c uint64_t es por consistencia; codigo JIT
     * normalmente ignora este valor y deposita el resultado en
     * @c regs[0] del ProcessVM antes del @c ret.
     */
    using JitFn = uint64_t (*)(vrt_proc *);

    /**
     * @brief Invoca @p fn pasando @p proc como argumento.
     *
     * El compilador host inlinea esta llamada hasta ser un @c call rax.
     * No hay copia de registros VM a regs nativos: el JIT-eado accede
     * @c proc->registers directamente cuando los necesita.
     *
     * En v1 NO preserva @c proc->rsp/rbp/registers: si la funcion JIT
     * spillea valores a stack, debe usar el stack NATIVO (no el stack
     * VM) y limpiar via @c add rsp, N antes del @c ret.
     *
     * @return el valor que la funcion JIT devuelva (normalmente 0,
     *         con el resultado real en @c proc->registers.regs[0]).
     */
    /* Sprint JIT-cross-fn 2026-06-01: setear TLS proc antes de entrar
     * al JIT.  Esto asegura que cualquier callback C invocado DESDE el
     * JIT-eated code (e.g. CALLN a vex_get_native_thunk para callbacks
     * Vex->C, o cualquier FFI nativa que necesite acceder al
     * ProcessVM) pueda hacer `runtime::get_current_executing_process()`
     * y obtener el proc correcto.  Coste: 1 store TLS (~1 ns) al
     * entrar al JIT.  Negligible vs el coste del JIT dispatch.
     *
     * Forward-decl para no incluir exception_runtime.h en este header
     * publico (rompe el include order). */
    extern "C" void runtime_set_current_executing_process_c(vrt_proc *proc);

    /* C2 reclaim (quiescencia): @c g_jit_reclaim_active es true SOLO cuando el
     * C2 tier-up esta habilitado (VESTA_C2_THRESHOLD>0); en el default es
     * false -> @c enter_jit no paga nada.  Cuando esta activo, @c jit_frame_enter
     * / @c jit_frame_exit llevan la cuenta de profundidad de frames JIT en la
     * pila nativa (los JIT->JIT directos quedan anidados dentro de un
     * @c enter_jit, asi que cuando la cuenta llega a 0 NO hay ningun frame JIT
     * en ninguna pila -> es seguro liberar el codigo C1 viejo de las funciones
     * tieradas).  @c jit_frame_exit drena el free-list pendiente al tocar 0.
     * Definidos en auto_jit.cpp. */
    extern bool g_jit_reclaim_active;
    void jit_frame_enter() noexcept;
    void jit_frame_exit()  noexcept;

    inline uint64_t enter_jit(JitFn fn, vrt_proc *proc) {
        runtime_set_current_executing_process_c(proc);
        if (g_jit_reclaim_active) {
            jit_frame_enter();
            const uint64_t r = fn(proc);
            jit_frame_exit();
            return r;
        }
        return fn(proc);
    }

    /**
     * @brief Stub para futura ruta JIT -> interprete.
     *
     * Cuando una funcion JIT decide salir al interprete (e.g. opcode
     * no implementado, deopt por type-miss en speculative IC), llama
     * este trampoline.  El trampoline:
     *
     *   - Salva los regs VM activos al stack VM.
     *   - Setea @c proc->rip al bytecode_pc del callsite.
     *   - Retorna al @c enter_jit, que en su frame restaura los regs
     *     nativos y vuelve al run_loop del scheduler.
     *
     * Stub en v1: simplemente retorna.  La implementacion real llega
     * en D.5 (tiered dispatch + OSR).
     */
    inline void return_from_jit(vrt_proc *proc, uint64_t bytecode_pc) {
        (void)proc; (void)bytecode_pc;
        /* TODO Phase D.5 */
    }

} // namespace jit

#endif // VESTA_JIT_INTERP_JIT_BRIDGE_H
