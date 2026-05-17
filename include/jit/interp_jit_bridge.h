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
    inline uint64_t enter_jit(JitFn fn, vrt_proc *proc) {
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
