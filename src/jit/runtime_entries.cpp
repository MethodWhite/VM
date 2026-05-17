/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/runtime_entries.cpp
 * @brief Resolucion estatica de @c jit::RuntimeEntries.
 *
 * Cada campo se inicializa con la direccion del simbolo @c vrt_*
 * correspondiente.  Como el binario enlaza @c libvesta_rt
 * estaticamente, las direcciones son conocidas en compile-time y el
 * compilador puede colapsar @c resolve() a stores constantes (lo
 * volveria inlinable trivial).
 *
 * El check @c all_resolved() existe para debug temprano: si alguien
 * olvida poblar un campo nuevo, el JIT veria un @c nullptr y crasheria
 * al primer call.  Mejor cazar el problema en init.
 */

#include "jit/runtime_entries.h"

namespace jit {

    void RuntimeEntries::resolve() {
        /* GC */
        gc_alloc          = &vrt_gc_alloc;
        gc_alloc_pinned   = &vrt_gc_alloc_pinned;
        gc_deref          = &vrt_gc_deref;
        gc_handle_for_ptr = &vrt_gc_handle_for_ptr;
        gc_drop           = &vrt_gc_drop;
        gc_addref         = &vrt_gc_addref;
        gc_release        = &vrt_gc_release;
        gc_write_barrier  = &vrt_gc_write_barrier;

        /* Monitores */
        monitor_enter      = &vrt_monitor_enter;
        monitor_exit       = &vrt_monitor_exit;
        monitor_wait       = &vrt_monitor_wait;
        monitor_notify     = &vrt_monitor_notify;
        monitor_notify_all = &vrt_monitor_notify_all;

        /* Excepciones */
        throw_fatal = &vrt_throw_fatal;
        tryenter    = &vrt_tryenter;
        tryleave    = &vrt_tryleave;

        /* FFI */
        invoke_native = &vrt_invoke_native;

        /* Dispatch dinamico (CALLVIRT desde JIT) */
        callvirt    = &vrt_callvirt;
        callm       = &vrt_callm;
        callclosure = &vrt_callclosure;
        calln       = &vrt_calln;

        /* VM memory + class registry */
        vm_read_u64  = &vrt_vm_read_u64;
        vm_write_u64 = &vrt_vm_write_u64;
        findclass    = &vrt_findclass;
        newobj         = &vrt_newobj;
        newobj_handle  = &vrt_newobj_handle;
        callvirt_ic    = &vrt_callvirt_ic;
        defclass     = &vrt_defclass;
        deffield     = &vrt_deffield;
        defmethod    = &vrt_defmethod;
        addadvice    = &vrt_addadvice;
        findmethod   = &vrt_findmethod;
        findfield    = &vrt_findfield;
        setmethdbg   = &vrt_setmethdbg;

        /* Safepoint */
        safepoint_poll    = &vrt_safepoint_poll;
        safepoint_handler = &vrt_safepoint_handler;
    }

    bool RuntimeEntries::all_resolved() const noexcept {
        return gc_alloc          && gc_alloc_pinned  && gc_deref          &&
               gc_handle_for_ptr && gc_drop          && gc_addref         &&
               gc_release        && gc_write_barrier &&
               monitor_enter     && monitor_exit     && monitor_wait      &&
               monitor_notify    && monitor_notify_all &&
               throw_fatal       && tryenter         && tryleave          &&
               invoke_native     && callvirt         &&
               callm             && callclosure     && calln            &&
               vm_read_u64       && vm_write_u64    &&
               findclass         && newobj          && defclass         &&
               deffield          && defmethod       && addadvice        &&
               findmethod        && findfield       && setmethdbg       &&
               safepoint_poll    && safepoint_handler;
    }

} // namespace jit
