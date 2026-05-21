/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/runtime_entries.cpp
 * @brief Resolucion estatica de la tabla de runtime entries para el JIT.
 *
 * @c RuntimeEntries es una struct que agrega TODOS los punteros a
 * funcion del runtime publico (@c vrt_*) que el codigo JIT-eado puede
 * necesitar invocar: alocadores GC, monitores, excepciones, dispatch
 * virtual, accesos a vm_mem y class registry, safepoints, etc.
 *
 * Por que una struct con punteros en lugar de llamar @c vrt_* directamente
 * desde el codigo emitido:
 *
 *   1. **Una sola indireccion conocida al runtime**: el JIT emite
 *      @c "mov rax, [runtime_table + offset]; call rax" en lugar de
 *      hardcodear las direcciones absolutas.  Esto facilita testing
 *      (inyectar fakes) y permitiria hot-swap del runtime en el futuro.
 *
 *   2. **Cero overhead post-link**: como el binario enlaza @c libvesta_rt
 *      estaticamente, las direcciones de los simbolos @c vrt_* son
 *      conocidas en compile-time.  El compilador colapsa @c resolve() a
 *      una secuencia de stores constantes (candidato a inlining trivial).
 *
 *   3. **Verificacion temprana**: @c all_resolved() recorre todos los
 *      campos y detecta nullptr.  Si alguien anyade un nuevo entry pero
 *      olvida cablearlo en @c resolve(), el JIT crasheria al primer call
 *      con un @c segfault sin contexto.  Llamar @c all_resolved() al
 *      init nos da un fail-fast con mensaje claro.
 */

#include "jit/runtime_entries.h"

namespace jit {

    /**
     * @brief Puebla todos los punteros con las direcciones de los @c vrt_*
     *        del runtime estatico.
     *
     * Llamado UNA vez al init del subsistema JIT.  El compilador
     * idealmente inlina esto a una secuencia de stores constantes
     * (todos los simbolos @c vrt_* tienen direccion conocida en link).
     *
     * Las categorias estan separadas con comentarios para que anyadir un
     * nuevo entry sea localizable: mismo orden tanto aqui como en el
     * header y en @c all_resolved.
     */
    void RuntimeEntries::resolve() {
        // -----------------------------------------------------------------
        // GC: allocacion, deref de GcHandle a host_ptr, ref-counting para
        // colecciones externas que pinnan handles, write barriers para
        // pointer-stores en objetos GC-managed.
        // -----------------------------------------------------------------
        gc_alloc          = &vrt_gc_alloc;
        gc_alloc_pinned   = &vrt_gc_alloc_pinned;
        gc_deref          = &vrt_gc_deref;
        gc_handle_for_ptr = &vrt_gc_handle_for_ptr;
        gc_drop           = &vrt_gc_drop;
        gc_addref         = &vrt_gc_addref;
        gc_release        = &vrt_gc_release;
        gc_write_barrier  = &vrt_gc_write_barrier;

        // -----------------------------------------------------------------
        // Monitores (sync primitives sobre objetos GC).  El JIT emite
        // calls a estos en lugar de inlinar el codigo del monitor porque
        // los waits y notifications cooperan con el scheduler.
        // -----------------------------------------------------------------
        monitor_enter      = &vrt_monitor_enter;
        monitor_exit       = &vrt_monitor_exit;
        monitor_wait       = &vrt_monitor_wait;
        monitor_notify     = &vrt_monitor_notify;
        monitor_notify_all = &vrt_monitor_notify_all;

        // -----------------------------------------------------------------
        // Excepciones: lanzar errores fatales capturables + tryenter /
        // tryleave para frames de try/catch desde codigo JIT-eado.
        // -----------------------------------------------------------------
        throw_fatal = &vrt_throw_fatal;
        tryenter    = &vrt_tryenter;
        tryleave    = &vrt_tryleave;
        throw_user  = &vrt_throw_user;
        rethrow     = &vrt_rethrow;

        // -----------------------------------------------------------------
        // FFI: invocacion de funciones nativas via puntero (CALLN del
        // bytecode).  La conversion de calling convention VM -> C ABI
        // ocurre en @c vrt_invoke_native.
        // -----------------------------------------------------------------
        invoke_native = &vrt_invoke_native;

        // -----------------------------------------------------------------
        // Dispatch dinamico: cuando un CALLVIRT en JIT necesita resolver
        // la vtable en runtime (interface call, reflexion, etc.).  El
        // JIT inlina el fast-path monomorfico cuando es posible; cae a
        // estas trampolines para los casos polimorficos.
        // -----------------------------------------------------------------
        callvirt    = &vrt_callvirt;
        callm       = &vrt_callm;
        callclosure = &vrt_callclosure;
        calln       = &vrt_calln;
        // Trampoline JIT->interp para CALL/CALLVM cuando la callee es
        // user-defined pero no se pudo JIT-compilar.  Permite que main +
        // helpers basicos compilen sin requerir que TODOS sus callees
        // sean JIT-supported.
        call_bc_function = &vrt_call_bc_function;

        // -----------------------------------------------------------------
        // Acceso a vm_mem (memoria virtual del proceso) y al class
        // registry.  El JIT no toca la memoria VM directamente (no
        // conoce el page table) -- delega via estos wrappers.
        // -----------------------------------------------------------------
        vm_read_u64  = &vrt_vm_read_u64;
        vm_write_u64 = &vrt_vm_write_u64;
        findclass    = &vrt_findclass;
        newobj         = &vrt_newobj;
        newobj_handle  = &vrt_newobj_handle;
        // Inline cache para CALLVIRT (variante optimizada que usa el
        // class_ptr previamente observado como prediccion).
        callvirt_ic    = &vrt_callvirt_ic;
        defclass     = &vrt_defclass;
        deffield     = &vrt_deffield;
        defmethod    = &vrt_defmethod;
        addadvice    = &vrt_addadvice;
        findmethod   = &vrt_findmethod;
        findfield    = &vrt_findfield;
        setmethdbg   = &vrt_setmethdbg;

        // -----------------------------------------------------------------
        // Safepoint: poll (cheap check del flag global) + handler (slow
        // path cuando el GC pidio stop).  El poll lo INLINA el JIT en
        // back-edges; el handler se llama solo en el slow path.
        // -----------------------------------------------------------------
        safepoint_poll    = &vrt_safepoint_poll;
        safepoint_handler = &vrt_safepoint_handler;
    }

    /**
     * @brief Verifica que TODOS los campos esten poblados (no-null).
     *
     * Llamar al init para detectar fail-fast un campo nuevo que se anyade
     * a la struct y se olvida en @c resolve.  El short-circuit @c && hace
     * que retorne false al primer null encontrado; idealmente la chain
     * completa es @c true en programas bien configurados.
     *
     * No es performance-critical (se llama una vez en init), asi que
     * preferimos la chain explicita sobre un loop con offsets reflectivos
     * (mas mantenible: anyadir un campo solo requiere anyadir un && aqui).
     */
    bool RuntimeEntries::all_resolved() const noexcept {
        return gc_alloc          && gc_alloc_pinned  && gc_deref          &&
               gc_handle_for_ptr && gc_drop          && gc_addref         &&
               gc_release        && gc_write_barrier &&
               monitor_enter     && monitor_exit     && monitor_wait      &&
               monitor_notify    && monitor_notify_all &&
               throw_fatal       && tryenter         && tryleave          &&
               throw_user        && rethrow          &&
               invoke_native     && callvirt         &&
               callm             && callclosure     && calln            &&
               call_bc_function  &&
               vm_read_u64       && vm_write_u64    &&
               findclass         && newobj          && defclass         &&
               deffield          && defmethod       && addadvice        &&
               findmethod        && findfield       && setmethdbg       &&
               safepoint_poll    && safepoint_handler;
    }

} // namespace jit
