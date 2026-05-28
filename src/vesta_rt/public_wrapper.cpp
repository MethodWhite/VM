/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file public_wrapper.cpp
 * @brief Implementacion de la API C estable @c vesta_rt/public.h.
 *
 * Cada wrapper es un thin shim que delega a la implementacion C++
 * interna.  Coste de la frontera: cero overhead (las funciones son
 * @c inline o llamadas directas; el compilador elimina la indirec-
 * cion cuando hay LTO o inlines).
 *
 * Los punteros opacos del header publico se castean a los tipos
 * reales del runtime:
 *
 *   vrt_proc*   -> runtime::ProcessVM*
 *   vrt_vm*     -> runtime::VM*
 *   vrt_class*  -> loader::ClassInfo*
 *   vrt_method* -> loader::MethodInfo*
 *   vrt_handle  -> gc::GcHandle (uint32_t)
 *
 * Por construccion (`extern "C"` + tipos POD-like), este modulo es
 * linkable contra cualquier toolchain (Cranelift, ensamblador a mano,
 * plugins terceros).
 */

#include "vesta_rt/public.h"
#include "vesta_rt/abi.h"

#include <cstdio>
#include <cstring>

#include "gc/gc_heap.h"
#include "jit/auto_jit.h"
#include "jit/interp_jit_bridge.h"
#include "loader/class_registry.h"
#include "loader/loader.h"
#include "loader/oop_types.h"
#include "runtime/decode_instruction.h"
#include "runtime/exception_runtime.h"
#include "runtime/manager_runtime.h"
#include "runtime/native_invoke.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"

#include <cstring>

namespace {
    /* Cast helpers locales para mantener legible el resto del codigo. */
    inline runtime::ProcessVM *as_proc(vrt_proc *p) noexcept {
        return reinterpret_cast<runtime::ProcessVM *>(p);
    }
    inline runtime::VM *as_vm(vrt_vm *v) noexcept {
        return reinterpret_cast<runtime::VM *>(v);
    }
    inline loader::ClassInfo *as_class(vrt_class *c) noexcept {
        return reinterpret_cast<loader::ClassInfo *>(c);
    }
} // namespace anonymous

extern "C" {

/* ----------------------------------------------------------------------- */
/* Version                                                                  */
/* ----------------------------------------------------------------------- */

uint32_t vrt_api_version(void) {
    /* version encoded como (major << 16) | (minor << 8) | patch */
    return (static_cast<uint32_t>(VRT_API_VERSION_MAJOR) << 16)
         | (static_cast<uint32_t>(VRT_API_VERSION_MINOR) << 8)
         | static_cast<uint32_t>(VRT_API_VERSION_PATCH);
}

/* ----------------------------------------------------------------------- */
/* GC                                                                       */
/* ----------------------------------------------------------------------- */

vrt_handle vrt_gc_alloc(vrt_proc *proc, size_t size) {
    if (!proc) return VRT_NULL_HANDLE;
    return as_proc(proc)->gc_heap.alloc(size);
}

vrt_handle vrt_gc_alloc_pinned(vrt_proc *proc, size_t size) {
    if (!proc) return VRT_NULL_HANDLE;
    return as_proc(proc)->gc_heap.alloc_pinned(size);
}

uint8_t *vrt_gc_deref(vrt_proc *proc, vrt_handle h) {
    if (!proc || h == VRT_NULL_HANDLE) return nullptr;
    return as_proc(proc)->gc_heap.deref(h);
}

vrt_handle vrt_gc_handle_for_ptr(vrt_proc *proc, const uint8_t *payload) {
    if (!proc || !payload) return VRT_NULL_HANDLE;
    return as_proc(proc)->gc_heap.handle_for_ptr(payload);
}

void vrt_gc_drop(vrt_proc *proc, vrt_handle h) {
    if (!proc || h == VRT_NULL_HANDLE) return;
    as_proc(proc)->gc_heap.drop(h);
}

void vrt_gc_addref(vrt_proc *proc, vrt_handle h) {
    if (!proc || h == VRT_NULL_HANDLE) return;
    as_proc(proc)->gc_heap.gc_addref(h);
}

void vrt_gc_release(vrt_proc *proc, vrt_handle h) {
    if (!proc || h == VRT_NULL_HANDLE) return;
    as_proc(proc)->gc_heap.gc_release(h);
}

void vrt_gc_write_barrier(vrt_proc *proc, vrt_handle old_handle) {
    if (!proc || old_handle == VRT_NULL_HANDLE) return;
    as_proc(proc)->gc_heap.write_barrier(old_handle);
}

/* ----------------------------------------------------------------------- */
/* Monitores                                                                */
/* ----------------------------------------------------------------------- */
/*
 * Los monitores en VestaVM viven en el ObjectHeader (owner_pid +
 * lock_depth).  GcHeap expone metodos C++ para adquirir/liberar; aqui
 * los wrappeamos con la signatura C estable.
 *
 * NOTE v1: el wake-up del proximo waiter requiere coordinacion con el
 * scheduler (vm.make_ready).  En v1 los wrappers solo implementan el
 * fast path (lock disponible o reentrante).  El slow path (cola de
 * espera + wake) seguira pasando por el bytecode interpretado hasta
 * Phase E (D.2).
 */

int32_t vrt_monitor_enter(vrt_proc *proc, vrt_handle obj) {
    if (!proc || obj == VRT_NULL_HANDLE) return 0;
    runtime::ProcessVM *p   = as_proc(proc);
    // Phase Z.11 ext: usamos el PID encoded (sched<<32|local) de 48 bits para
    // evitar la colision local_pid cross-scheduler.
    const uint64_t      pid = (static_cast<uint64_t>(p->pid.scheduler_id) << 32)
                              | static_cast<uint64_t>(p->pid.local_pid);
    return p->gc_heap.monitor_try_acquire(obj, pid) ? 1 : 0;
}

void vrt_monitor_exit(vrt_proc *proc, vrt_handle obj) {
    if (!proc || obj == VRT_NULL_HANDLE) return;
    runtime::ProcessVM *p   = as_proc(proc);
    const uint64_t      pid = (static_cast<uint64_t>(p->pid.scheduler_id) << 32)
                              | static_cast<uint64_t>(p->pid.local_pid);
    (void)p->gc_heap.monitor_release(obj, pid);
    /* TODO Phase E (D.2): si monitor_release devuelve un waiter,
     * llamar vm.make_ready(next).  Por ahora el bytecode handler
     * en exec_instruction_sync.cpp se encarga. */
}

void vrt_monitor_wait(vrt_proc *proc, vrt_handle obj) {
    /* Slow path: queda como stub.  El JIT en v1 cae al interprete
     * via el trampoline @c jit_to_interp para llamar al bytecode
     * MONWAIT.  En Phase E expondremos un wait completo aqui. */
    (void)proc; (void)obj;
}

void vrt_monitor_notify(vrt_proc *proc, vrt_handle obj) {
    if (!proc || obj == VRT_NULL_HANDLE) return;
    /* monitor_pop_waiter devuelve UN waiter o GC_NULL_HANDLE; el
     * caller debe propagarlo al scheduler.  Stub minimo. */
    (void)as_proc(proc)->gc_heap.monitor_pop_waiter(obj);
}

void vrt_monitor_notify_all(vrt_proc *proc, vrt_handle obj) {
    if (!proc || obj == VRT_NULL_HANDLE) return;
    (void)as_proc(proc)->gc_heap.monitor_pop_all_waiters(obj);
}

/* ----------------------------------------------------------------------- */
/* Excepciones                                                              */
/* ----------------------------------------------------------------------- */

void vrt_throw_fatal(vrt_proc *proc, uint32_t kind, const char *message) {
    if (!proc) return;
    runtime::throw_fatal(as_proc(proc), kind, message);
}

/* Forward decl: do_throw vive en exec_instruction_oop.cpp con C++ mangling.
 * NO debe estar en el extern "C" envolvente; usamos un alias que pueda ser
 * llamado desde las funciones C. */
void vrt_internal_do_throw(runtime::ProcessVM *vm, uint64_t exc_handle);

void vrt_tryenter(vrt_proc *proc, uint64_t handler_pc, vrt_class *type_class) {
    /* Replica EXACTA de @c exec_instr_tryenter sin la parte de
     * @c reductions_remaining (irrelevante en frame JIT: el batch ya
     * lo controla el scheduler enclosing).
     *
     * Aloca un ExceptionFrame en heap raw + lo inicializa con:
     *   - handler_pc: direccion del catch (absoluta VM)
     *   - type:        ClassInfo* del catch (nullptr = catch-all)
     *   - saved_rsp/rbp/frame_stack: snapshot para descartar
     *     pushes del regalloc + frames OOP inacabados durante throw.
     *   - saved_regs[0..15]: snapshot de R0..R15 para que el catch
     *     vea el mismo estado que el try entry (igual que C/C++ exc). */
    if (!proc) return;
    runtime::ProcessVM *p = as_proc(proc);

    /* Acquire del free list si tiene frames recicladas; sino alloca.
     * El free list lo popula @c tryleave (bytecode + JIT inline),
     * eliminando malloc/free pressure y leak por programas con many
     * try/catch en loops. */
    runtime::ProcessVM::ExceptionFrame *ef;
    if (p->exc_free_list != nullptr) {
        ef = p->exc_free_list;
        p->exc_free_list = ef->prev;
    } else {
        ef = new runtime::ProcessVM::ExceptionFrame();
    }
    ef->handler_pc        = handler_pc;
    ef->type              = reinterpret_cast<loader::ClassInfo *>(type_class);
    ef->saved_rsp         = p->registers.stack_pointer.qword();
    ef->saved_rbp         = p->registers.base_pointer.qword();
    ef->saved_frame_stack = reinterpret_cast<uint64_t>(p->frame_stack);
    for (int i = 0; i < 16; ++i) {
        ef->saved_regs[i] = p->registers.regs[i].qword();
    }
    ef->prev = p->exc_frame_stack;
    p->exc_frame_stack = ef;
}

void vrt_tryleave(vrt_proc *proc) {
    /* Pop del tope + push al free list para reciclar.  El JIT inlinea
     * la misma logica en 7 instrucciones x86-64 cuando @c exc_frame_stack_offset
     * + @c exc_free_list_offset estan configurados (~3x mas rapido que
     * llegar aqui via call).  Esta version C es fallback para bytecode
     * interp + JIT en modo testing sin offsets. */
    if (!proc) return;
    runtime::ProcessVM *p = as_proc(proc);
    if (p->exc_frame_stack == nullptr) return;
    auto *top = p->exc_frame_stack;
    p->exc_frame_stack = top->prev;
    /* Push al free list (reusar campo prev). */
    top->prev = p->exc_free_list;
    p->exc_free_list = top;
}

void vrt_throw_user(vrt_proc *proc, uint64_t exc_handle) {
    /* Delega a do_throw.  Nunca retorna normalmente -- do_throw modifica
     * RIP/RSP/RBP/regs del proceso y el JIT-eado debe detectar el cambio
     * via... bueno, no puede.  El throw rompe el flujo normal: tras este
     * call, el JIT frame queda en estado inconsistente.
     *
     * v1: confiamos en que do_throw setea el RIP al handler.  El JIT
     * NO debe ejecutar nada despues de esta llamada (el lowering del
     * frontend marca el bloque como terminator post-throw, asi que no
     * habra mas instrucciones JIT en ese bloque).
     *
     * BUG conocido v1: si el handler PC esta en codigo bytecode (no JIT),
     * el control regresa al interp via el scheduler.  Pero el JIT frame
     * sigue activo en host stack -> stale frame.  Cuando el handler
     * eventualmente RET-ea, salta a la direccion de retorno bytecode
     * (sentinel del trampoline o frame OOP) y todo funciona.  Si el handler
     * RET-ea a la JIT frame original... unwinding cruzado no soportado.
     *
     * Para v1 SOLO soportamos: throw desde JIT cuando el catch esta TAMBIEN
     * en codigo bytecode (caso comun: el frontend emite catch como bloque
     * de bytecode, NO como JIT-callable function). */
    if (!proc) return;
    vrt_internal_do_throw(as_proc(proc), exc_handle);
}

void vrt_rethrow(vrt_proc *proc) {
    /* Relanza la excepcion activa.  El frontend la emite tras hacer monexit
     * en el handler de un synchronized: el flujo es body -> throw -> catch
     * (frame del synchronized) -> monexit -> rethrow -> outer catch.
     *
     * Mismas limitaciones que vrt_throw_user. */
    if (!proc) return;
    runtime::ProcessVM *p = as_proc(proc);
    vrt_internal_do_throw(p, p->current_exception);
}

/* ----------------------------------------------------------------------- */
/* FFI nativo                                                               */
/* ----------------------------------------------------------------------- */

uint64_t vrt_invoke_native(void *fn, uint64_t argc, vrt_proc *proc) {
    if (!fn || !proc) return 0;
    return runtime::invoke_native_unchecked(fn, argc, as_proc(proc));
}

/* ----------------------------------------------------------------------- */
/* Dispatch dinamico (CALLVIRT desde JIT)                                  */
/* ----------------------------------------------------------------------- */

/* Forward decl: definido mas abajo (compartido entre vrt_callvirt y vrt_callm). */
static uint64_t vrt_run_method_in_interp(runtime::ProcessVM *p,
                                         loader::MethodInfo *mi);

uint64_t vrt_callvirt(vrt_proc *proc, uint8_t *obj_payload, uint32_t vtbl_idx) {
    if (!proc || !obj_payload) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "callvirt: obj payload nulo");
        }
        return 0;
    }
    runtime::ProcessVM *p = as_proc(proc);

    /* Convencion bytecode/Vex: obj_payload APUNTA al ObjectHeader (NO al
     * payload de campos).  El gcderef del bytecode devuelve `addr + sizeof(GcHeader)`
     * que ES la direccion del ObjectHeader; y los campos siguen DESPUES del header
     * a offset >= sizeof(ObjectHeader).  Tratar obj_payload directamente como
     * ObjectHeader* es lo consistente con el resto del runtime. */
    loader::ObjectHeader *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_payload);
    loader::ClassInfo *cls = hdr->class_ptr;
    if (!cls || vtbl_idx >= cls->vtable_size) {
        runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
            "callvirt: clase nula o vtable_idx fuera de rango");
        return 0;
    }
    loader::MethodInfo *method = cls->vtable[vtbl_idx];
    if (!method) {
        runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
            "callvirt: vtable slot vacio (metodo abstracto?)");
        return 0;
    }

    /* Si el metodo aun no esta JIT-compilado, intentar compilarlo ahora.
     * Esto fuerza el camino lazy: cualquier callvirt desde JIT compila
     * tambien la callee (con threshold=1 efectivo) para que la proxima
     * invocacion sea directa. */
    if (method->jit_code == nullptr) {
        const uint32_t saved = method->invocation_count;
        method->invocation_count = jit::g_jit_threshold;
        jit::maybe_compile_method(p, method);
        /* maybe_compile_method puede haber seteado invocation_count
         * a UINT32_MAX si fallo - no resetear en ese caso. */
        if (method->jit_code == nullptr) {
            method->invocation_count = saved;
        }
    }

    /* Si el metodo tiene advices (AOP @Before/@After/@Around), NO podemos
     * llamar directo al JIT-eated body porque eso salta la cadena
     * advice_chain.  Caemos al bytecode interp que SI recorre la cadena
     * correctamente (ver exec_instr_callvirt slow path). */
    if (method->jit_code != nullptr && method->advice_chain == nullptr) {
        /* Dispatch directo a codigo nativo.  VM_ABI: args ya en
         * proc->registers.regs[1..N] (el caller los puso antes de invocar
         * vrt_callvirt).  Solo necesitamos asegurar R1 = obj_payload. */
        p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
        jit::JitFn fn = reinterpret_cast<jit::JitFn>(method->jit_code);
        return jit::enter_jit(fn, proc);
    }

    /* Metodo no compilable (raw_asm complejo, float ops, synchronized, etc).
     * FALLBACK 2026-05-16: ejecutar el bytecode del metodo en mini-interp
     * sincronico.  Mismo flujo que vrt_callm fallback.  Lento pero CORRECTO. */
    p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
    return vrt_run_method_in_interp(p, method);
}

/* INLINE CACHE variant: igual que vrt_callvirt pero al final escribe
 * (class_ptr, jit_code) en el slot de cache pasado.  Si el slot ya tiene
 * el mismo class_ptr (hit), el JIT-eated code ya hizo dispatch directo
 * sin llamar esta funcion.  Solo se invoca en MISS path o primera
 * invocacion (slot a 0).  Layout del slot (16 bytes):
 *   +0  [8] cached_class_ptr
 *   +8  [8] cached_jit_code (o vrt_run_method_in_interp_addr para fallback)
 */
uint64_t vrt_callvirt_ic(vrt_proc *proc, uint8_t *obj_payload,
                          uint32_t vtbl_idx, uint64_t ic_slot_addr) {
    if (!proc || !obj_payload) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "callvirt_ic: obj payload nulo");
        }
        return 0;
    }
    runtime::ProcessVM *p = as_proc(proc);
    loader::ObjectHeader *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_payload);
    loader::ClassInfo *cls = hdr->class_ptr;
    if (!cls || vtbl_idx >= cls->vtable_size) {
        runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
            "callvirt_ic: clase nula o vtable_idx fuera de rango");
        return 0;
    }
    loader::MethodInfo *method = cls->vtable[vtbl_idx];
    if (!method) {
        runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
            "callvirt_ic: vtable slot vacio (metodo abstracto?)");
        return 0;
    }

    if (method->jit_code == nullptr) {
        const uint32_t saved = method->invocation_count;
        method->invocation_count = jit::g_jit_threshold;
        jit::maybe_compile_method(p, method);
        if (method->jit_code == nullptr) {
            method->invocation_count = saved;
        }
    }

    /* Actualizar el slot IC: solo cuando hay jit_code Y NO hay advices
     * (esos requieren fallback siempre).  Si advice_chain != null o
     * jit_code == null, NO actualizar el slot -- de esa forma el slot
     * permanece a 0 y el fast path inline siempre va al miss path. */
    if (ic_slot_addr != 0 && method->jit_code != nullptr
     && method->advice_chain == nullptr) {
        uint64_t *slot = reinterpret_cast<uint64_t *>(ic_slot_addr);
        slot[0] = reinterpret_cast<uint64_t>(cls);
        slot[1] = reinterpret_cast<uint64_t>(method->jit_code);
    }

    if (method->jit_code != nullptr && method->advice_chain == nullptr) {
        p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
        jit::JitFn fn = reinterpret_cast<jit::JitFn>(method->jit_code);
        return jit::enter_jit(fn, proc);
    }

    p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
    return vrt_run_method_in_interp(p, method);
}

/* Sentinel para el fallback bytecode sincronico: cuando un metodo
 * invocado desde JIT no se puede JIT-compilar, ejecutamos su bytecode
 * en un mini-loop interno hasta que el RET del frame haga PC =
 * VRT_BYTECODE_RET_SENTINEL.  Usamos un valor alto (>4GB, fuera del
 * rango de VAs del .velb) para que sea distinguible.  Si por algun
 * motivo el bytecode itera fuera del codigo (e.g. corrupcion), el
 * proceso saldra por EVT_HALT/EVT_ERROR antes. */
static constexpr uint64_t VRT_BYTECODE_RET_SENTINEL = 0xFEFEFEFE00000000ULL;

/* Helper compartido: ejecuta el bytecode VM en @p p comenzando en
 * @p bc_va, en un mini run-loop sincronico, hasta que el RET final pop
 * el sentinel y salte a el.  No pushea frame OOP (uso para funciones
 * libres / closures; los metodos virtuales usan vrt_run_method_in_interp
 * que SI pushea el frame OOP).
 *
 * Args y resultado pasan por la convencion VM_ABI estandar:
 *   - args en proc->registers.regs[1..N] (puestos por el caller)
 *   - argc en proc->registers.regs[15]
 *   - retorno en proc->registers.regs[0] (devuelto a este helper)
 *
 * Restaura rip/rsp/frame_stack/decoded_ptr al estado previo asi que
 * el JIT caller puede continuar normalmente.
 */
static uint64_t vrt_run_bc_at(runtime::ProcessVM *p, uint64_t bc_va) {
    const uint64_t saved_rip = p->registers.rip.raw();
    const uint64_t saved_rsp = p->registers.stack_pointer.qword();
    auto *saved_frame_stack  = p->frame_stack;
    auto *saved_decoded_ptr  = p->decoded_ptr;

    const uint64_t new_rsp = saved_rsp - 8;
    p->vm_mem.write_u64_fast(new_rsp, VRT_BYTECODE_RET_SENTINEL);
    p->registers.stack_pointer.qword(new_rsp);
    p->registers.rip.qword(bc_va);
    p->decoded_ptr = &p->icache[0];
    p->decoded_ptr->pc = UINT64_MAX;

    const uint64_t MAX_ITERS = 100'000'000;
    uint64_t iters = 0;
    while (iters++ < MAX_ITERS) {
        if (p->registers.rip.raw() == VRT_BYTECODE_RET_SENTINEL) break;
        if (p->state == runtime::HALT || p->state == runtime::DEAD) break;
        if (p->err_thread != runtime::THREAD_NO_ERROR) break;
        runtime::decode_instruction(p);
        runtime::vm_event ev = runtime::execute_instruction(p);
        if (ev == runtime::EVT_HALT || ev == runtime::EVT_ERROR) break;
        if (ev == runtime::EVT_IO_WAIT) {
            runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
                "vrt trampoline JIT->interp: IO_WAIT no soportado en sync interp");
            break;
        }
    }

    p->registers.rip.qword(saved_rip);
    p->registers.stack_pointer.qword(saved_rsp);
    p->frame_stack = saved_frame_stack;
    p->decoded_ptr = saved_decoded_ptr;
    return p->registers.regs[0].qword();
}

/* Mini-interp sincronico: ejecuta el bytecode del metodo @p mi en el
 * proceso @p p.  Args ya estan en regs[1..N] (calling convention).
 * Pushea un frame OOP + ret_addr=SENTINEL, salta a mi->code_vaddr, y
 * loop hasta que RIP vuelva al SENTINEL (RET final del metodo).
 * Retorna regs[0] al terminar.
 *
 * Restaura RIP, RSP, frame_stack y decoded_ptr al estado previo para
 * que el JIT caller pueda continuar normalmente. */
static uint64_t vrt_run_method_in_interp(runtime::ProcessVM *p,
                                         loader::MethodInfo *mi) {
    /* Salvar estado del proceso. */
    const uint64_t saved_rip   = p->registers.rip.raw();
    const uint64_t saved_rsp   = p->registers.stack_pointer.qword();
    auto *saved_frame_stack    = p->frame_stack;
    auto *saved_decoded_ptr    = p->decoded_ptr;

    /* Push ret_addr sentinel al stack VM. */
    const uint64_t new_rsp = saved_rsp - 8;
    p->vm_mem.write_u64_fast(new_rsp, VRT_BYTECODE_RET_SENTINEL);
    p->registers.stack_pointer.qword(new_rsp);

    /* Push frame OOP (necesario para que callvirt/proceed funcionen
     * desde dentro del metodo si los usa). */
    auto *frame = p->frame_pool.acquire();
    frame->prev            = p->frame_stack;
    frame->method          = mi;
    frame->return_pc       = VRT_BYTECODE_RET_SENTINEL;
    frame->frame_base      = saved_rsp;
    frame->proceed_target  = nullptr;
    p->frame_stack         = frame;

    /* Saltar al code del metodo. */
    p->registers.rip.qword(mi->code_vaddr);

    /* Reset decoded_ptr para forzar fresh decode. */
    p->decoded_ptr = &p->icache[0];
    p->decoded_ptr->pc = UINT64_MAX;  /* invalidar para que decode no haga HIT erroneo */

    /* Mini run-loop: ejecutar hasta que el frame retorne al sentinel
     * o el proceso entre en HALT/DEAD/error.  Max iters defensivo. */
    const uint64_t MAX_ITERS = 100'000'000;
    uint64_t iters = 0;
    while (iters++ < MAX_ITERS) {
        if (p->registers.rip.raw() == VRT_BYTECODE_RET_SENTINEL) break;
        if (p->state == runtime::HALT || p->state == runtime::DEAD) break;
        if (p->err_thread != runtime::THREAD_NO_ERROR) break;
        runtime::decode_instruction(p);
        runtime::vm_event ev = runtime::execute_instruction(p);
        if (ev == runtime::EVT_HALT || ev == runtime::EVT_ERROR) break;
        if (ev == runtime::EVT_IO_WAIT) {
            /* No podemos esperar IO sincronicamente desde JIT.
             * Abortar el fallback con error. */
            runtime::throw_fatal(p, VESTA_FATAL_ILLEGAL_INSTRUCTION,
                "vrt_callm fallback: IO_WAIT no soportado en sync interp");
            break;
        }
    }

    /* Restaurar RSP (descartar el sentinel push si quedo).  El bytecode
     * RET normal lo habria pop-eado; si paramos por error, lo limpiamos
     * aqui.  Lo mismo para frame_stack. */
    p->registers.rip.qword(saved_rip);
    p->registers.stack_pointer.qword(saved_rsp);
    if (p->frame_stack == frame) {
        /* No se hizo pop -- liberar el frame manualmente. */
        p->frame_stack = saved_frame_stack;
        p->frame_pool.release(frame);
    }
    p->decoded_ptr = saved_decoded_ptr;

    return p->registers.regs[0].qword();
}

uint64_t vrt_callm(vrt_proc *proc, uint8_t *obj_payload, void *method) {
    if (!proc || !method) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "callm: method o proc nulo");
        }
        return 0;
    }
    runtime::ProcessVM *p = as_proc(proc);
    loader::MethodInfo *mi = reinterpret_cast<loader::MethodInfo *>(method);

    /* Si el metodo no esta JIT-compilado, intentar compilar on-demand. */
    if (mi->jit_code == nullptr) {
        const uint32_t saved = mi->invocation_count;
        mi->invocation_count = jit::g_jit_threshold;
        jit::maybe_compile_method(p, mi);
        if (mi->jit_code == nullptr) {
            mi->invocation_count = saved;
        }
    }

    if (mi->jit_code != nullptr) {
        if (obj_payload != nullptr) {
            p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
        }
        jit::JitFn fn = reinterpret_cast<jit::JitFn>(mi->jit_code);
        return jit::enter_jit(fn, proc);
    }

    /* JIT fallo (e.g. raw_asm complejo, callee no soportado).
     * FALLBACK 2026-05-16: ejecutar el bytecode del metodo en un
     * mini-loop sincronico.  Lento pero CORRECTO (no devuelve 0
     * silenciosamente). */
    if (obj_payload != nullptr) {
        p->registers.regs[1].qword(reinterpret_cast<uint64_t>(obj_payload));
    }
    return vrt_run_method_in_interp(p, mi);
}

uint64_t vrt_callclosure(vrt_proc *proc, uint64_t fn_addr, uint64_t env_addr) {
    if (!proc || fn_addr == 0) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "callclosure: fn_addr o proc nulo");
        }
        return 0;
    }
    runtime::ProcessVM *p = as_proc(proc);
    /* Convencion de closures: env_addr en R14 antes de la llamada. */
    p->registers.regs[14].qword(env_addr);

    /* fn_addr puede ser:
     *   (a) una direccion bytecode VM (< 4GB; helper sintetico
     *       __lambda_<N> que el frontend emite con @Absolute("code.X")).
     *   (b) un ptr nativo (> 4GB) si el closure fue eager-compilado.
     *
     * Heuristica de rango: VM vaddr < 4GB, host ptr > 4GB en x64.
     * BUG FIX 2026-05-16: el codigo anterior siempre asumia (b) y
     * crasheaba con segfault al saltar a una VM addr como si fuera host.
     */
    if (fn_addr > 0x100000000ULL) {
        /* Host ptr a codigo JIT. */
        jit::JitFn fn = reinterpret_cast<jit::JitFn>(fn_addr);
        return jit::enter_jit(fn, proc);
    }
    /* VM bytecode addr: ejecutar via mini-interp delegando al helper
     * compartido vrt_run_bc_at.  El env_addr ya esta en R14 (linea
     * arriba), por lo que el lambda body lo encontrara como espera la
     * convencion de closures. */
    return vrt_run_bc_at(p, fn_addr);
}

uint64_t vrt_call_bc_function(vrt_proc *proc, uint64_t bc_entry_va) {
    /* Trampoline JIT->interp para CALL/CALLVM cuando la callee no se
     * pudo JIT-compilar.  El selector emite esta llamada en lugar de
     * marcar el caller como unsupported, permitiendo que main y otros
     * metodos JIT-compilen aunque algunas callees caigan a interp.
     *
     * Args ya stagedos en proc->registers.regs[1..N] por el caller JIT
     * (convencion VM_ABI), argc en R15.  Resultado en R0 tras retorno. */
    if (!proc || bc_entry_va == 0) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "vrt_call_bc_function: bc_entry_va nulo");
        }
        return 0;
    }
    return vrt_run_bc_at(as_proc(proc), bc_entry_va);
}

uint64_t vrt_calln(vrt_proc *proc, const char *lib_name, const char *fn_name) {
    /* CALLN desde JIT: stub que lanza fatal.  La resolucion de
     * (lib, fn) -> fn_ptr requiere acceso al Loader y al cache de
     * native_imports.  Phase D.3-G+ implementara esto plenamente.
     * Por ahora cualquier funcion con CALLN no se compila (el selector
     * marca unsupported antes de llegar aqui via warn).
     *
     * Alternativa para programs que necesitan FFI desde JIT: usar
     * ffi_open/ffi_sym/ffi_call (CALLNI dinamico) que tampoco esta
     * soportado todavia, o no usar -m jit. */
    (void)lib_name;
    (void)fn_name;
    if (proc) {
        runtime::throw_fatal(as_proc(proc), VESTA_FATAL_ILLEGAL_INSTRUCTION,
            "calln desde JIT no implementado en v1 (Phase D.3-G+)");
    }
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Safepoint                                                                */
/* ----------------------------------------------------------------------- */

void vrt_safepoint_poll(vrt_proc *proc) {
    /* Camino lento explicito (rara vez usado): el JIT normalmente emite
     * el poll inline como @c cmp byte [rbx], 0; jne handler.  Esta
     * funcion existe para fallback / debugging. */
    if (!proc) return;
    runtime::ProcessVM *p = as_proc(proc);
    if (p->safepoint_flag) {
        vrt_safepoint_handler(proc);
    }
}

void vrt_safepoint_handler(vrt_proc *proc) {
    /* D.2-foundation v1: implementacion minima del handler.
     *
     * El handler corre cuando el GC del propio proceso quiere pausar
     * para hacer stack scan.  En esta fase:
     *
     *   1. Limpiamos el flag para que el JIT no quede en bucle al
     *      retornar (en cada poll comprobara y vera 0).
     *   2. Reservamos espacio para que la integracion GC (Phase
     *      D.2-integration) capture RIP/RBP aqui y coordine con el GC.
     *
     * Como en D.2-foundation aun no hay GC integration, el handler
     * simplemente limpia el flag y retorna.  La proxima iteracion del
     * codigo JIT continuara normalmente. */
    if (!proc) return;
    runtime::ProcessVM *p = as_proc(proc);
    p->safepoint_flag = 0;
    /* TODO D.2-integration:
     *   - Capturar RBP del caller (necesita assembly inline o
     *     llamada con __builtin_frame_address).
     *   - Notificar al GC que llegamos al safepoint.
     *   - Esperar sobre condvar hasta que el GC termine.
     *   - Restaurar y retornar. */
}

/* ----------------------------------------------------------------------- */
/* Introspeccion                                                            */
/* ----------------------------------------------------------------------- */

vrt_vm *vrt_proc_vm(vrt_proc *proc) {
    if (!proc) return nullptr;
    /* ProcessVM.scheduler -> Scheduler.vm_reference */
    return reinterpret_cast<vrt_vm *>(&as_proc(proc)->scheduler.vm_reference);
}

uint64_t vrt_proc_pid(vrt_proc *proc) {
    if (!proc) return 0;
    const runtime::ProcessVM *p = as_proc(proc);
    /* PID encoded: (scheduler_id << 32) | local_pid */
    return (static_cast<uint64_t>(p->pid.scheduler_id) << 32)
         |  static_cast<uint64_t>(p->pid.local_pid);
}

/* ----------------------------------------------------------------------- */
/* Acceso a memoria VM (Phase D.3-G)                                       */
/* ----------------------------------------------------------------------- */

uint64_t vrt_vm_read_u64(vrt_proc *proc, uint64_t vaddr) {
    if (!proc) return 0;
    return as_proc(proc)->vm_mem.read_u64_fast(vaddr);
}

void vrt_vm_write_u64(vrt_proc *proc, uint64_t vaddr, uint64_t value) {
    if (!proc) return;
    as_proc(proc)->vm_mem.write_u64_fast(vaddr, value);
}

/* ----------------------------------------------------------------------- */
/* Class registry runtime entries (Phase D.3-G)                            */
/* ----------------------------------------------------------------------- */

vrt_class *vrt_findclass(vrt_proc *proc, uint64_t params_vaddr) {
    if (!proc) return nullptr;
    runtime::ProcessVM *p = as_proc(proc);
    /* FindClassParams: +0 name_vaddr [8], +8 name_len [4]. */
    uint64_t name_vaddr = p->vm_mem.read_u64_fast(params_vaddr);
    uint32_t name_len = 0;
    p->vm_mem.read_bytes(params_vaddr + 8, &name_len, 4);
    if (name_len == 0 || name_len > 4096) return nullptr;
    /* Leer el nombre desde vm_mem a buffer local. */
    char buf[4097];
    p->vm_mem.read_bytes(name_vaddr, buf, name_len);
    buf[name_len] = '\0';
    /* Lookup en el class registry del Loader. */
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    auto *cls = reg.find_class(buf);
    return reinterpret_cast<vrt_class *>(cls);
}

uint8_t *vrt_newobj(vrt_proc *proc, vrt_class *cls) {
    if (!proc || !cls) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "newobj: clase nula");
        }
        return nullptr;
    }
    runtime::ProcessVM *p = as_proc(proc);
    loader::ClassInfo *ci = reinterpret_cast<loader::ClassInfo *>(cls);
    /* gc_heap.alloc devuelve un GcHandle; deref para obtener host_ptr al payload. */
    const uint32_t handle = p->gc_heap.alloc(ci->instance_size);
    if (handle == VRT_NULL_HANDLE) {
        runtime::throw_fatal(p, VESTA_FATAL_NULL_POINTER, "newobj: OOM");
        return nullptr;
    }
    uint8_t *payload = p->gc_heap.deref(handle);
    if (!payload) return nullptr;
    /* Inicializar header: class_ptr = ci, flags = OBJ_FLAG_GC_OWNED.
     * Replica exacto del bytecode exec_instr_newobj. */
    auto *hdr     = reinterpret_cast<loader::ObjectHeader *>(payload);
    hdr->class_ptr = ci;
    hdr->flags     = loader::OBJ_FLAG_GC_OWNED;
    /* El resto del header viene zeroed por el alloc. */
    /* Retornar el host_ptr al ObjectHeader (convencion bytecode/Vex:
     * gcderef devuelve addr+sizeof(GcHeader) = ObjectHeader start, NO
     * FIELDS start).  Los accesos a campos en Vex usan offset >=
     * sizeof(ObjectHeader) desde este puntero. */
    return payload;
}

vrt_handle vrt_newobj_handle(vrt_proc *proc, vrt_class *cls) {
    /* Misma logica que vrt_newobj + vrt_gc_handle_for_ptr combinados.
     * Optimizacion clave: gc_heap.alloc YA devuelve el handle, asi que
     * NO necesitamos hacer el lookup de ptr_to_handle_ que haria
     * vrt_gc_handle_for_ptr.  Ahorra ~30-50 ns por @c new X(). */
    if (!proc || !cls) {
        if (proc) {
            runtime::throw_fatal(as_proc(proc), VESTA_FATAL_NULL_POINTER,
                "newobj: clase nula");
        }
        return VRT_NULL_HANDLE;
    }
    runtime::ProcessVM *p = as_proc(proc);
    loader::ClassInfo *ci = reinterpret_cast<loader::ClassInfo *>(cls);
    const uint32_t handle = p->gc_heap.alloc(ci->instance_size);
    if (handle == VRT_NULL_HANDLE) {
        runtime::throw_fatal(p, VESTA_FATAL_NULL_POINTER, "newobj: OOM");
        return VRT_NULL_HANDLE;
    }
    uint8_t *payload = p->gc_heap.deref(handle);
    if (!payload) return VRT_NULL_HANDLE;
    auto *hdr     = reinterpret_cast<loader::ObjectHeader *>(payload);
    hdr->class_ptr = ci;
    hdr->flags     = loader::OBJ_FLAG_GC_OWNED;
    return handle;
}

/* ===================================================================== */
/* vrt_register_alloc                                      */
/* ===================================================================== */
/* Llamado por codigo JIT-eated tras INLINE bump-pointer alloc para
 * registrar el handle del objeto recien alocado.  El JIT inlineea:
 *   - bump-pointer (lee/escribe @c nursery_bump_)
 *   - init GcHeader (size, color, gen)
 *   - init ObjectHeader (class_ptr, flags)
 *   - memset payload
 *
 * Pero la creacion del handle (push a @c handles_ + insert a @c
 * ptr_to_handle_) es demasiado compleja para inlinear -- delegada
 * a este helper.  Coste medido: ~7-8 ns (vs ~25 ns de @c vrt_newobj
 * full).
 *
 * @param proc ProcessVM cuyo gc_heap se va a actualizar.
 * @param raw  Puntero host al GcHeader del objeto recien alocado.
 * @return Handle creado o @c VRT_NULL_HANDLE si fallo.
 */
uint64_t vrt_register_alloc(vrt_proc *proc, uint8_t *raw) {
    if (!proc || !raw) return VRT_NULL_HANDLE;
    runtime::ProcessVM *p = as_proc(proc);
    return p->gc_heap.register_alloc(raw);
}

vrt_class *vrt_defclass(vrt_proc *proc, uint64_t params_vaddr) {
    if (!proc) return nullptr;
    runtime::ProcessVM *p = as_proc(proc);
    /* DefClassParams ABI (32 bytes):
     *   +0  name_addr     [8]
     *   +8  name_len      [4]
     *   +12 flags         [4]
     *   +16 super_class   [8]  ptr o 0
     *   +24 _reserved     [8]  */
    struct Params { uint64_t name_addr; uint32_t name_len; uint32_t flags;
                    uint64_t super_ptr; uint64_t _rsv; };
    Params pr{};
    p->vm_mem.read_bytes(params_vaddr, &pr, sizeof(pr));
    if (pr.name_len == 0 || pr.name_len > 4096) return nullptr;
    char buf[4097];
    p->vm_mem.read_bytes(pr.name_addr, buf, pr.name_len);
    buf[pr.name_len] = '\0';
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    loader::ClassInfo *super_cls = reinterpret_cast<loader::ClassInfo *>(pr.super_ptr);
    /* define_class signature: (name, super, interfaces[], fields[], methods[], flags) */
    auto *cls = reg.define_class(buf, super_cls, {}, {}, {}, pr.flags);
    return reinterpret_cast<vrt_class *>(cls);
}

int32_t vrt_deffield(vrt_proc *proc, vrt_class *cls, uint64_t params_vaddr) {
    if (!proc || !cls) return 0;
    runtime::ProcessVM *p = as_proc(proc);
    /* DefFieldParams (32 bytes):
     *   +0  name_addr   [8]
     *   +8  name_len    [4]
     *   +12 kind        [1]
     *   +13 access      [1]
     *   +14 is_static   [1]
     *   +15 _pad
     *   +16 size_bytes  [4]
     *   +20 _pad2
     *   +24 type_class  [8]
     */
    struct Params { uint64_t name_addr; uint32_t name_len; uint8_t kind; uint8_t access;
                    uint8_t is_static; uint8_t _pad; uint32_t size_bytes; uint32_t _pad2;
                    uint64_t type_class; };
    Params pr{};
    p->vm_mem.read_bytes(params_vaddr, &pr, sizeof(pr));
    if (pr.name_len == 0 || pr.name_len > 4096) return 0;
    char buf[4097];
    p->vm_mem.read_bytes(pr.name_addr, buf, pr.name_len);
    buf[pr.name_len] = '\0';
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    loader::FieldDecl fd{};
    fd.name = buf;
    fd.kind = static_cast<loader::FieldKind>(pr.kind);
    fd.access = static_cast<loader::FieldAccess>(pr.access);
    fd.is_static = pr.is_static != 0;
    fd.size_bytes = pr.size_bytes;
    fd.type_class = reinterpret_cast<loader::ClassInfo *>(pr.type_class);
    auto *ci = reinterpret_cast<loader::ClassInfo *>(cls);
    return reg.add_field(ci, fd) ? 1 : 0;
}

uint32_t vrt_defmethod(vrt_proc *proc, vrt_class *cls, uint64_t params_vaddr) {
    if (!proc || !cls) return UINT32_MAX;
    runtime::ProcessVM *p = as_proc(proc);
    /* DefMethodParams (40 bytes):
     *   +0  name_addr      [8]
     *   +8  name_len       [4]
     *   +12 descriptor_len [4]
     *   +16 descriptor_addr[8]
     *   +24 code_vaddr     [8]
     *   +32 flags          [8]
     */
    struct Params { uint64_t name_addr; uint32_t name_len; uint32_t descriptor_len;
                    uint64_t descriptor_addr; uint64_t code_vaddr; uint64_t flags; };
    Params pr{};
    p->vm_mem.read_bytes(params_vaddr, &pr, sizeof(pr));
    if (pr.name_len == 0 || pr.name_len > 4096) return UINT32_MAX;
    char buf[4097];
    p->vm_mem.read_bytes(pr.name_addr, buf, pr.name_len);
    buf[pr.name_len] = '\0';
    char desc_buf[4097];
    if (pr.descriptor_len > 0 && pr.descriptor_len <= 4096) {
        p->vm_mem.read_bytes(pr.descriptor_addr, desc_buf, pr.descriptor_len);
        desc_buf[pr.descriptor_len] = '\0';
    } else {
        desc_buf[0] = '\0';
    }
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    loader::MethodDecl md{};
    md.name = buf;
    md.descriptor = desc_buf;
    md.code_vaddr = pr.code_vaddr;
    md.flags = pr.flags;
    auto *ci = reinterpret_cast<loader::ClassInfo *>(cls);
    return reg.add_method(ci, md);
}

int32_t vrt_addadvice(vrt_proc *proc, void *target_method, void *advice_method, uint8_t kind) {
    if (!proc || !target_method || !advice_method) return 0;
    runtime::ProcessVM *p = as_proc(proc);
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    auto *target = reinterpret_cast<loader::MethodInfo *>(target_method);
    auto *advice = reinterpret_cast<loader::MethodInfo *>(advice_method);
    return reg.add_advice(target, kind, advice) ? 1 : 0;
}

/* Helper compartido: detecta si @p ptr es una direccion HOST (>4GB) o
 * VM vaddr (<=4GB) y lee @p size bytes a @p dst.  El JIT pasa host_ptr
 * desde ALLOCA stack-nativo; el bytecode interp pasa VM vaddr.  La
 * heuristica de rango funciona porque el VM mem se mapea siempre a
 * direcciones <4GB y el host stack siempre vive >4GB en x64.
 *
 * Coste: 1 cmp + 1 branch (~1 ciclo predicho).  Despreciable comparado
 * con el FFI overhead que ya tiene cada CALLN.
 */
static inline void read_params_unified(runtime::ProcessVM *p,
                                       uint64_t ptr, void *dst, size_t size) {
    if (ptr > 0x100000000ULL) {
        /* Host stack ptr -> memcpy directo. */
        std::memcpy(dst, reinterpret_cast<const void *>(ptr), size);
    } else {
        p->vm_mem.read_bytes(ptr, dst, size);
    }
}

void *vrt_findmethod(vrt_proc *proc, uint64_t params_vaddr) {
    if (!proc) return nullptr;
    runtime::ProcessVM *p = as_proc(proc);
    /* FindMethodParams (24 bytes): +0 class_ptr, +8 name_addr, +16 name_len */
    struct Params { uint64_t class_ptr; uint64_t name_addr; uint32_t name_len; uint32_t _pad; };
    Params pr{};
    read_params_unified(p, params_vaddr, &pr, sizeof(pr));
    if (!pr.class_ptr || pr.name_len == 0 || pr.name_len > 4096) return nullptr;
    char buf[4097];
    /* name_addr sigue siendo VM vaddr (static_data del .velb). */
    p->vm_mem.read_bytes(pr.name_addr, buf, pr.name_len);
    buf[pr.name_len] = '\0';
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    auto *ci = reinterpret_cast<loader::ClassInfo *>(pr.class_ptr);
    return reg.find_method(ci, buf);
}

void *vrt_findfield(vrt_proc *proc, uint64_t params_vaddr) {
    if (!proc) return nullptr;
    runtime::ProcessVM *p = as_proc(proc);
    struct Params { uint64_t class_ptr; uint64_t name_addr; uint32_t name_len; uint32_t _pad; };
    Params pr{};
    read_params_unified(p, params_vaddr, &pr, sizeof(pr));
    if (!pr.class_ptr || pr.name_len == 0 || pr.name_len > 4096) return nullptr;
    char buf[4097];
    p->vm_mem.read_bytes(pr.name_addr, buf, pr.name_len);
    buf[pr.name_len] = '\0';
    auto &reg = p->scheduler.vm_reference.loader_public.class_registry();
    auto *ci = reinterpret_cast<loader::ClassInfo *>(pr.class_ptr);
    return reg.find_field(ci, buf);
}

void vrt_setmethdbg(vrt_proc *proc, uint64_t params_vaddr) {
    if (!proc || params_vaddr == 0) return;
    runtime::ProcessVM *p = as_proc(proc);
    /* Mismo layout que SetMethDebugParams en exec_instr_setmethdbg. */
    struct DebugParams {
        uint64_t method_ptr;
        uint64_t file_addr;
        uint32_t file_len;
        uint32_t start_line;
    };
    DebugParams sp{};
    p->vm_mem.read_bytes(params_vaddr, &sp, sizeof(sp));
    if (sp.method_ptr == 0) return;
    char fbuf[256];
    size_t flen = (size_t)sp.file_len;
    if (flen > 255) flen = 255;
    if (sp.file_addr != 0 && flen > 0) {
        p->vm_mem.read_bytes(sp.file_addr, fbuf, flen);
    }
    fbuf[flen] = '\0';
    runtime::register_method_debug(
        reinterpret_cast<loader::MethodInfo *>(sp.method_ptr),
        fbuf, flen, sp.start_line);
}

} /* extern "C" */

/* ------------------------------------------------------------------------- */
/* Alias C++ namespaced para invocar @c runtime::do_throw desde las funciones
 * C wrapper @c vrt_throw_user / @c vrt_rethrow.  No esta en extern "C" para
 * preservar el name-mangling de C++.                                        */
/* ------------------------------------------------------------------------- */
namespace runtime {
    void do_throw(ProcessVM *vm, uint64_t exception_ptr);
}

void vrt_internal_do_throw(runtime::ProcessVM *vm, uint64_t exc_handle) {
    runtime::do_throw(vm, exc_handle);
}
