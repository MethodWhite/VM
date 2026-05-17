/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file exec_instruction_oop.cpp
 * @brief Implementacion de instrucciones del sistema de objetos (0x00 0xD0..0xEB).
 *
 * Convencion de punteros en registros:
 *   - Todos los punteros a ClassInfo*, MethodInfo*, FieldInfo* y al ObjectHeader
 *     de un objeto son PUNTEROS HOST (uint64_t casteado) almacenados en registros
 *     de proposito general.
 *   - Para objetos GC: se debe hacer GCDEREF primero para obtener el host_ptr
 *     al ObjectHeader, y luego usar ese puntero con CALLVIRT/THROW/etc.
 *   - Para objetos raw (NEWOBJRAW): el puntero al ObjectHeader se obtiene
 *     directamente de R00 tras la instruccion.
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/exception_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

/* hook JIT en CALLVIRT fast path. */
#include "jit/interp_jit_bridge.h"
#include "vesta_rt/public.h"

namespace runtime {

    // =========================================================================
    //  Helpers internos
    // =========================================================================

    /**
     * @brief Comprueba si @p obj_class es igual a @p target o hereda de el.
     *
     * Realiza una busqueda BFS recursiva sobre la cadena de superclases e
     * interfaces.  La recursion es aceptable porque las jerarquias suelen
     * ser cortas en la practica.
     *
     * @param obj_class Clase del objeto a comprobar.
     * @param target    Clase objetivo de la comprobacion.
     * @return true si obj_class es target o hereda de el.
     */
    static bool is_instance_of(const loader::ClassInfo *obj_class,
                                const loader::ClassInfo *target) {
        if (obj_class == nullptr || target == nullptr) return false; // punteros nulos -> falso
        if (obj_class == target) return true;                        // coincidencia directa

        // comprobar superclases
        for (size_t i = 0; i < obj_class->super_count; ++i) {
            if (is_instance_of(obj_class->supers[i], target)) return true;
        }
        // comprobar interfaces implementadas
        for (size_t i = 0; i < obj_class->interface_count; ++i) {
            if (is_instance_of(obj_class->interfaces[i], target)) return true;
        }
        return false; // no encontrado en toda la jerarquia
    }

    /**
     * @brief Implementacion comun de THROW y RETHROW.
     *
     * Recorre la cadena de frames buscando un handler compatible con la clase
     * de la excepcion lanzada:
     *   - Si encuentra handler: restaura frame_stack, pone el puntero en R00 y salta.
     *   - Si no encuentra handler: mata el proceso con EVT_ERROR.
     *
     * @param vm            Proceso virtual sobre el que se lanza la excepcion.
     * @param exception_ptr Puntero host al ObjectHeader de la excepcion (0 = invalido).
     */
    void do_throw(ProcessVM *vm, uint64_t exception_ptr) {
        if (exception_ptr == 0) {
            // excepcion nula -> error de segmentacion
            vm->err_thread = THREAD_SEGMENTATION_FAULT;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        auto *exc_hdr = reinterpret_cast<loader::ObjectHeader *>(exception_ptr);
        loader::ClassInfo *exc_class = exc_hdr->class_ptr; // clase de la excepcion

        // guardar la excepcion activa para que RETHROW pueda relanzarla
        vm->current_exception = exception_ptr;

        // --- comprobar primero la pila de frames TRYENTER (frames ligeros de alto nivel) ---
        ProcessVM::ExceptionFrame *ef = vm->exc_frame_stack;
        while (ef != nullptr) {
            bool matches = (ef->type == nullptr) || is_instance_of(exc_class, ef->type);
            if (matches) {
                uint64_t handler_addr   = ef->handler_pc; // guardar antes de liberar
                uint64_t saved_rsp      = ef->saved_rsp;
                uint64_t saved_rbp      = ef->saved_rbp;
                auto    *saved_fs       = (loader::FrameHeader *)(uintptr_t)ef->saved_frame_stack;
                // Snapshot de R0..R15 que el tryenter capturo.  Restauramos
                // antes de saltar al handler para que las variables vivas
                // del catch (incluido `this` y parametros del metodo) tengan
                // los valores correctos -- mismo estado que el try entry.
                // Sin esto, los regs corruptos por el try-body dejaban
                // las vars con valores stale (caso clasico: catch que
                // hace `this.foo()` -> CALLVIRT null porque r1 era stale).
                uint64_t saved_regs[16];
                for (int i = 0; i < 16; ++i) saved_regs[i] = ef->saved_regs[i];

                // desapilar todos los frames TRYENTER hasta el handler encontrado
                while (vm->exc_frame_stack != nullptr && vm->exc_frame_stack != ef) {
                    ProcessVM::ExceptionFrame *tmp = vm->exc_frame_stack;
                    vm->exc_frame_stack = tmp->prev;
                    delete tmp; // liberar frame apilado por TRYENTER
                }
                vm->exc_frame_stack = ef->prev; // el handler se consume al saltar
                delete ef;

                // Unwind del CPU stack y de frame_stack al estado
                // del tryenter.  Esto descarta:
                //   - Pushes del regalloc dentro del try-body que no
                //     llegaron a sus pop por el throw -> evita corrupcion
                //     de registros vivos al volver al merge.
                //   - Frames de calls anidados (callvirt/callm) que no
                //     retornaron normalmente -> evita stack frame leak
                //     y pop de retorno desde direccion incorrecta.
                vm->registers.stack_pointer.qword(saved_rsp);
                vm->registers.base_pointer.qword(saved_rbp);
                while (vm->frame_stack != nullptr && vm->frame_stack != saved_fs) {
                    loader::FrameHeader *tmp = vm->frame_stack;
                    vm->frame_stack = tmp->prev;
                    vm->frame_pool.release(tmp); // fix13
                }
                // Restaurar R1..R15 (R0 lo sobreescribimos con la
                // excepcion abajo).  Esto es el cambio CRITICO que hace
                // que las vars del catch vean valores correctos.
                for (int i = 1; i < 16; ++i) {
                    vm->registers.regs[i].qword(saved_regs[i]);
                }

                // convencion: R00 contiene el puntero al objeto excepcion
                vm->registers.regs[R00].qword(exception_ptr);
                vm->registers.rip.qword(handler_addr); // saltar al handler
                vm->decoded_ptr->flags_info.did_jump = true;
                return;
            }
            ef = ef->prev;
        }

        loader::FrameHeader *frame = vm->frame_stack; // continuar con la cadena de FrameHeaders

        while (frame != nullptr) {
            loader::MethodInfo *method = frame->method;
            if (method == nullptr) {
                frame = frame->prev; // frame sin metodo, subir al siguiente
                continue;
            }

            // calcular el offset del PC respecto al inicio del metodo
            uint64_t pc_offset = vm->registers.rip.raw() - method->code_vaddr;

            for (size_t i = 0; i < method->handler_count; ++i) {
                const loader::HandlerException &h = method->handlers[i];

                // verificar que el PC esta dentro del rango cubierto por el handler
                if (pc_offset < h.start_pc || pc_offset >= h.end_pc) continue;

                // verificar compatibilidad de tipo (nullptr = catch-all / finally)
                bool matches = (h.type == nullptr)
                             || is_instance_of(exc_class, h.type);

                if (matches) {
                    // desapilar todos los frames intermedios hasta el handler
                    loader::FrameHeader *cur = vm->frame_stack;
                    while (cur != nullptr && cur != frame) {
                        loader::FrameHeader *tmp = cur;
                        cur = cur->prev;
                        vm->frame_pool.release(tmp); // fix13
                    }
                    vm->frame_stack = frame; // restaurar el frame del handler

                    // convencion: R00 contiene el puntero al objeto excepcion
                    vm->registers.regs[R00].qword(exception_ptr);

                    // saltar al handler dentro del metodo
                    vm->registers.rip.qword(method->code_vaddr + h.handler_pc);
                    vm->decoded_ptr->flags_info.did_jump = true; // notificar el salto
                    return;
                }
            }

            // handler no encontrado en este frame: subir al anterior
            loader::FrameHeader *done = frame;
            frame = frame->prev;
            vm->frame_pool.release(done); // fix13
        }

        // excepcion no capturada en ningun frame: matar el proceso
        vm->frame_stack = nullptr;
        vm->scheduler.on_event(EVT_ERROR);
    }

    // =========================================================================
    //  0xD0 NEWOBJRAW reg_classinfo, reg_size -> R0 = host_ptr
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion NEWOBJRAW: aloca un objeto raw sin GC.
     *
     * Aloca un bloque de memoria bruta usando el raw_allocator del proceso.
     * El tamano se toma del segundo registro si es distinto de cero; de lo
     * contrario se usa instance_size de la clase.  El ObjectHeader se inicializa
     * con OBJ_FLAG_RAW_OWNED para distinguirlo de los objetos GC.
     *
     * @param vm    Proceso virtual que ejecuta NEWOBJRAW.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (size).
     */
    void exec_instr_newobjraw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1; // registro con puntero a ClassInfo
        const uint8_t r_sz  = instr.data_instruction.reg_data.reg2; // registro con tamano (0 = usar instance_size)

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr) {
            vm->registers.regs[R00].qword(0); // clase nula -> puntero nulo
            return;
        }

        // si r_sz != 0, usar el tamano del registro; si no, usar el de la clase
        size_t alloc_size = (r_sz != 0)
            ? static_cast<size_t>(vm->registers.regs[r_sz].qword())
            : static_cast<size_t>(cls->instance_size);

        uint64_t ptr = vm->raw_alloc.alloc(alloc_size); // alocar con el allocator bruto
        if (ptr != 0) {
            auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(ptr);
            hdr->class_ptr = cls;                                          // enlazar la clase
            hdr->flags     = loader::OBJ_FLAG_RAW_OWNED;                   // marcar como raw (no GC)
            hdr->hash_code = static_cast<uint32_t>(ptr & 0xFFFFFFFF);     // hash basado en la direccion
        }

        vm->registers.regs[R00].qword(ptr); // devolver el puntero host en R00
    }

    // =========================================================================
    //  0xD1 CALLVIRT reg_obj, vtable_idx
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CALLVIRT: llamada virtual a un metodo de objeto.
     *
     * Resuelve el metodo en tiempo de ejecucion a traves de la vtable de la clase
     * del objeto.  Empuja un FrameHeader en frame_stack para que THROW/RETHROW
     * puedan localizar handlers, y tambien empuja la direccion de retorno en la
     * pila convencional (compatible con RET).
     *
     * @param vm    Proceso virtual que ejecuta CALLVIRT.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto) y reg_data.reg2 (vtable_idx).
     */
    void exec_instr_callvirt(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj    = instr.data_instruction.reg_data.reg1; // registro con el puntero al objeto
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2; // indice en la vtable

        const uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        if (__builtin_expect(obj_ptr == 0, 0)) {
            // throw_fatal capturable con try/catch.  Si no hay
            // handler activo, ruta antigua (mata el proceso).
            runtime::throw_fatalf(vm, runtime::FATAL_NULL_POINTER,
                "CALLVIRT: deref de objeto null (r%u, vtbl_idx=%u)",
                (unsigned)r_obj, (unsigned)vtbl_idx);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        loader::ClassInfo *cls = hdr->class_ptr; // obtener la clase del objeto

        // ---------- Inline cache (monomorphic) ----------
        // En la mayoria de programas, el call site siempre ve la MISMA clase.
        // Cacheamos (cls -> method) en el icache entry.  Cache hit = saltar
        // bounds check + indirect load de la vtable.
        loader::MethodInfo *method;
        if (__builtin_expect(cls == instr.cached_class
                              && instr.cached_method != nullptr, 1)) {
            method = static_cast<loader::MethodInfo *>(instr.cached_method);
        } else {
            // Cache miss: resolver via vtable y actualizar.  La primera
            // invocacion de cada call site cae aqui; las siguientes son hits.
            if (__builtin_expect(cls == nullptr
                                  || vtbl_idx >= cls->vtable_size
                                  || cls->vtable == nullptr, 0)) {
                runtime::throw_fatalf(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                    "CALLVIRT: clase invalida o vtable_idx fuera de rango (idx=%u, size=%u)",
                    (unsigned)vtbl_idx,
                    cls ? (unsigned)cls->vtable_size : 0u);
                return;
            }
            method = cls->vtable[vtbl_idx];
            if (__builtin_expect(method == nullptr || method->code_vaddr == 0, 0)) {
                runtime::throw_fatalf(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                    "CALLVIRT: metodo abstracto o sin implementacion (vtbl_idx=%u)",
                    (unsigned)vtbl_idx);
                return;
            }
            // Cachear para la proxima invocacion (mutable: const-correct).
            instr.cached_class  = cls;
            instr.cached_method = method;
        }

        const uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr); // PC de retorno

        // -----------------------------------------------------------------
        // AOP: si el metodo tiene cadena de advices, recorrerla y construir
        // la secuencia efectiva [b1, b2, ..., bN, M, a1, a2, ..., aK].  Cada
        // paso se traduce en (a) un FrameHeader empujado (para excepciones),
        // (b) un return_pc empujado en la pila (para que RET salte al
        // siguiente paso), todo en orden inverso al de ejecucion (LIFO).
        // Si advice_chain es NULL o no hay BEFORE/AFTER, fast path identico
        // al original (overhead cero).
        // -----------------------------------------------------------------

        // Helper local para empujar (frame + return_pc) atomicamente.  Se
        // invoca varias veces consecutivas para construir la cadena AOP.
        // El parametro opcional @c around_target marca el frame como un
        // advice AROUND con un target a invocar via @c proceed (A.5.4 ext).
        //
        // NOTA: NO escribimos ret_to al stack VM.  El slot reservado por
        // `rsp -= 8` queda sin inicializar, lo cual es seguro porque:
        //  (a) RET de este callvirt usa frame->return_pc directo (detectado
        //      por RSP == frame_base - 8), nunca lee del slot.
        //  (b) Si el method body hace un callvm anidado, ese callvm
        //      sobreescribe su propio slot (rsp -= 8 de nuevo + write).  El
        //      RET de ese callvm anidado lee de su slot, no del nuestro.
        // El write_bytes que estaba aqui (~30 ns) era trabajo desperdiciado.
        auto push_step = [&](loader::MethodInfo *m, uint64_t ret_to,
                             loader::MethodInfo *around_target = nullptr) {
            const uint64_t cur_rsp = vm->registers.stack_pointer.qword();
            auto *frame            = vm->frame_pool.acquire();
            frame->prev            = vm->frame_stack;
            frame->method          = m;
            frame->return_pc       = ret_to;
            frame->frame_base      = cur_rsp;
            frame->proceed_target  = around_target;
            vm->frame_stack        = frame;
            vm->registers.stack_pointer.qword(cur_rsp - 8);
        };

        if (method->advice_chain == nullptr) {
            // Fast path: dispatch directo sin advices, overhead cero.
            //
            // HOT PATH OPT:  si method->jit_code ya esta
            // seteado, salt directo al JIT SIN incrementar counter ni
            // llamar al hook.  Esto ahorra ~2-3 ns por callvirt en hot
            // loops (de los ~10ns totales del fast path).  El counter
            // ya sirvio para disparar la compilacion; tras compile no
            // tiene utilidad (futuras phases podrian usarlo para PGO
            // pero hoy no se consulta).  El hook es no-op cuando
            // jit_code != null, asi que skip es seguro.
            if (__builtin_expect(method->jit_code != nullptr, 1)) {
                jit::JitFn fn = reinterpret_cast<jit::JitFn>(method->jit_code);
                (void)jit::enter_jit(fn, reinterpret_cast<vrt_proc *>(vm));
                vm->registers.rip.qword(ret_addr);
                vm->decoded_ptr->flags_info.did_jump = true;
                return;
            }
            // Slow path: jit_code no seteado.  Incrementar counter y
            // disparar auto-JIT si procede.  Tras compile exitoso,
            // siguientes callvirts iran por el fast path.
            ++method->invocation_count;
            if (g_callvirt_post_hook != nullptr) {
                g_callvirt_post_hook(vm, method);
            }
            if (method->jit_code != nullptr) {
                jit::JitFn fn = reinterpret_cast<jit::JitFn>(method->jit_code);
                (void)jit::enter_jit(fn, reinterpret_cast<vrt_proc *>(vm));
                vm->registers.rip.qword(ret_addr);
                vm->decoded_ptr->flags_info.did_jump = true;
                return;
            }
            push_step(method, ret_addr);
            vm->registers.rip.qword(method->code_vaddr);
            vm->decoded_ptr->flags_info.did_jump = true;
            return;
        }

        // Recolectar BEFORE/AFTER/AROUND en orden de declaracion.  Para
        //  ext soportamos UN AROUND (el primero); sucesivos AROUNDs
        // se ignoran silenciosamente (multi-AROUND nesting requiere stack
        // de targets, deferido).
        loader::MethodInfo *befores[16];
        loader::MethodInfo *afters [16];
        loader::MethodInfo *around = nullptr;
        size_t n_b = 0, n_a = 0;
        for (loader::AdviceEntry *e = method->advice_chain; e != nullptr; e = e->next) {
            if (e->kind == loader::ADVICE_BEFORE && n_b < 16) {
                befores[n_b++] = e->advice_method;
            } else if (e->kind == loader::ADVICE_AFTER && n_a < 16) {
                afters[n_a++]  = e->advice_method;
            } else if (e->kind == loader::ADVICE_AROUND && around == nullptr) {
                around = e->advice_method;
            }
        }

        if (n_b == 0 && n_a == 0 && around == nullptr) {
            push_step(method, ret_addr);
            vm->registers.rip.qword(method->code_vaddr);
            vm->decoded_ptr->flags_info.did_jump = true;
            return;
        }

        // Si hay AROUND, M se reemplaza por el advice AROUND en la
        // secuencia.  El advice puede invocar @c proceed para llamar al
        // M original (almacenado en frame.proceed_target).  Si el advice
        // no llama a proceed, M nunca se ejecuta (semantica esperada).
        loader::MethodInfo *m_slot = (around != nullptr) ? around : method;
        loader::MethodInfo *proceed_tgt = (around != nullptr) ? method : nullptr;

        loader::MethodInfo *seq[33];
        size_t n_seq = 0;
        for (size_t i = 0; i < n_b; ++i) seq[n_seq++] = befores[i];
        seq[n_seq++] = m_slot;
        const size_t m_slot_index = n_seq - 1;
        for (size_t i = 0; i < n_a; ++i) seq[n_seq++] = afters[i];

        // Push del ultimo al primero.  Cada paso recibe su own return_pc;
        // el slot de M (o de AROUND) recibe ademas proceed_target.
        push_step(seq[n_seq - 1], ret_addr,
                  (n_seq - 1 == m_slot_index) ? proceed_tgt : nullptr);
        for (size_t i = n_seq - 1; i > 0; --i) {
            const size_t idx = i - 1;
            push_step(seq[idx], seq[i]->code_vaddr,
                      (idx == m_slot_index) ? proceed_tgt : nullptr);
        }

        vm->registers.rip.qword(seq[0]->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xFE PROCEED (sin operandos)
    //
    //  Invoca el target original de un advice AROUND.  Lee el campo
    //  @c proceed_target del frame actual (puesto por CALLVIRT/CALLM al
    //  detectar AROUND).  Equivale a un @c callm r1, proceed_target con
    //  los registros actuales (r1=this, args en r2..rN).
    //
    //  Si el frame actual no es un AROUND (proceed_target == nullptr),
    //  fallamos con THREAD_ILLEGAL_INSTRUCTION (proceed solo es valido
    //  dentro del body de un advice AROUND activo).
    // =========================================================================

    void exec_instr_proceed(ProcessVM *vm, const DecodedInstr &instr) {
        loader::FrameHeader *cur = vm->frame_stack;
        if (cur == nullptr || cur->proceed_target == nullptr) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "PROCEED: instruccion fuera de un frame AROUND");
            return;
        }
        loader::MethodInfo *target = cur->proceed_target;
        if (target->code_vaddr == 0) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "PROCEED: target del AROUND no tiene codigo");
            return;
        }
        const uint64_t ret_addr = vm->registers.rip.raw()
                                + static_cast<uint64_t>(instr.flags_info.size_instr);

        // Push frame para target (sin nuevo proceed_target: el target
        // no es AROUND de si mismo).  La calling convention preserva
        // r1=this y args ya en su sitio.
        auto *frame            = vm->frame_pool.acquire(); // A.34.fix13
        frame->prev            = vm->frame_stack;
        frame->method          = target;
        frame->return_pc       = ret_addr;
        frame->frame_base      = vm->registers.stack_pointer.qword();
        frame->proceed_target  = nullptr;
        vm->frame_stack        = frame;
        vm->registers.stack_pointer.qword(
            vm->registers.stack_pointer.qword() - 8);
        // Mantener el write para callvm anidado dentro del callee.
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(),
                                &ret_addr, 8);
        vm->registers.rip.qword(target->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xFD CALLM reg_obj, reg_method
    //
    //  Como CALLVIRT, pero el segundo operando es un MethodInfo* directo
    //  (no un vtable_idx).  Util para dispatch dinamico:
    //    - Llamadas a traves de un tipo de interfaz (lookup por nombre).
    //    - Reflexion: invocar un MethodInfo* obtenido via getMethod.
    //    - Callbacks generados por el runtime sin conocer el slot vtable.
    //
    //  Formato FIXED_4: [0xFD][ctrl][reg_byte][_]
    //    reg1 = r_obj (objeto receptor, host_ptr a ObjectHeader)
    //    reg2 = r_method (MethodInfo* obtenido via findmethod/getmethod)
    //
    //  El objeto se valida solo para nullness; el ClassInfo del header NO
    //  se consulta porque el method ya viene resuelto.  Esto permite
    //  invocar advices, callbacks, lambdas, etc.
    //
    //  IMPORTANTE: la cadena AOP advice_chain del MethodInfo SI se
    //  recorre, igual que en CALLVIRT, para mantener semantica uniforme.
    //  Sin esto un dispatcher dinamico se saltaria los aspectos.
    // =========================================================================

    void exec_instr_callm(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj    = instr.data_instruction.reg_data.reg1;
        const uint8_t r_method = instr.data_instruction.reg_data.reg2;

        const uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        if (__builtin_expect(obj_ptr == 0, 0)) {
            runtime::throw_fatal(vm, runtime::FATAL_NULL_POINTER,
                "CALLM: deref de objeto null");
            return;
        }
        auto *method = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[r_method].qword());
        if (__builtin_expect(method == nullptr || method->code_vaddr == 0, 0)) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CALLM: MethodInfo nulo o sin codigo");
            return;
        }
        const uint64_t ret_addr = vm->registers.rip.raw()
                                + static_cast<uint64_t>(instr.flags_info.size_instr);

        // Helper local: empuja frame + return_pc (mismo patron que CALLVIRT).
        // Sin write a stack: RET usa frame->return_pc directo (matching por
        // frame_base).  Ver explicacion extendida en CALLVIRT::push_step.
        auto push_step = [&](loader::MethodInfo *m, uint64_t ret_to) {
            const uint64_t cur_rsp = vm->registers.stack_pointer.qword();
            auto *frame       = vm->frame_pool.acquire(); // A.34.fix13
            frame->prev       = vm->frame_stack;
            frame->method     = m;
            frame->return_pc  = ret_to;
            frame->frame_base = cur_rsp;
            frame->proceed_target = nullptr;
            vm->frame_stack   = frame;
            vm->registers.stack_pointer.qword(cur_rsp - 8);
        };

        // Fast path: sin advices, dispatch directo.
        if (method->advice_chain == nullptr) {
            push_step(method, ret_addr);
            vm->registers.rip.qword(method->code_vaddr);
            vm->decoded_ptr->flags_info.did_jump = true;
            return;
        }

        // Slow path: recorrer advice_chain como en CALLVIRT.  Construir
        // [BEFORE..., M, AFTER...] e instalar la cadena en pila.
        loader::MethodInfo *befores[16];
        loader::MethodInfo *afters [16];
        size_t n_b = 0, n_a = 0;
        for (loader::AdviceEntry *e = method->advice_chain;
             e != nullptr; e = e->next) {
            if      (e->kind == loader::ADVICE_BEFORE && n_b < 16) befores[n_b++] = e->advice_method;
            else if (e->kind == loader::ADVICE_AFTER  && n_a < 16) afters [n_a++] = e->advice_method;
        }
        if (n_b == 0 && n_a == 0) {
            push_step(method, ret_addr);
            vm->registers.rip.qword(method->code_vaddr);
            vm->decoded_ptr->flags_info.did_jump = true;
            return;
        }
        loader::MethodInfo *seq[33];
        size_t n_seq = 0;
        for (size_t i = 0; i < n_b; ++i) seq[n_seq++] = befores[i];
        seq[n_seq++] = method;
        for (size_t i = 0; i < n_a; ++i) seq[n_seq++] = afters[i];
        push_step(seq[n_seq - 1], ret_addr);
        for (size_t i = n_seq - 1; i > 0; --i) {
            push_step(seq[i - 1], seq[i]->code_vaddr);
        }
        vm->registers.rip.qword(seq[0]->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xD2 CALLSUPER reg_classinfo, vtable_idx
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CALLSUPER: llamada a un metodo de la superclase.
     *
     * Similar a CALLVIRT pero resuelve el metodo en la vtable de la clase
     * indicada por el registro (no la del objeto receptor), permitiendo
     * llamadas super() explicitas.
     *
     * @param vm    Proceso virtual que ejecuta CALLSUPER.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (vtable_idx).
     */
    void exec_instr_callsuper(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls    = instr.data_instruction.reg_data.reg1; // registro con puntero a ClassInfo
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2; // indice en la vtable

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || vtbl_idx >= cls->vtable_size || cls->vtable == nullptr) {
            runtime::throw_fatalf(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CALLSUPER: ClassInfo invalido o vtable_idx fuera de rango (idx=%u)",
                (unsigned)vtbl_idx);
            return;
        }

        loader::MethodInfo *method = cls->vtable[vtbl_idx]; // resolver metodo en la clase indicada
        if (method == nullptr || method->code_vaddr == 0) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CALLSUPER: metodo abstracto o sin implementacion");
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr);

        // crear FrameHeader para soporte de excepciones
        const uint64_t cur_rsp = vm->registers.stack_pointer.qword();
        auto *frame       = vm->frame_pool.acquire(); // A.34.fix13
        frame->prev       = vm->frame_stack;
        frame->method     = method;
        frame->return_pc  = ret_addr;
        frame->frame_base = cur_rsp;
        frame->proceed_target = nullptr;
        vm->frame_stack   = frame;

        // RSP -= 8 (reserva slot).  No escribimos ret_addr: RET de este
        // callsuper usa frame->return_pc directo (mismo patron que callvirt).
        vm->registers.stack_pointer.qword(cur_rsp - 8);

        // saltar al metodo de la superclase
        vm->registers.rip.qword(method->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xD3 THROW reg_obj
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion THROW: lanza una excepcion con el objeto indicado.
     *
     * @param vm    Proceso virtual que ejecuta THROW.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto excepcion).
     */
    void exec_instr_throw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1; // registro con el objeto excepcion
        uint64_t exception_ptr = vm->registers.regs[r_obj].qword();
        do_throw(vm, exception_ptr); // delegar en el helper comun
    }

    // =========================================================================
    //  0xD4 RETHROW
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion RETHROW: relanza la excepcion activa.
     *
     * Usa el campo current_exception del proceso para relanzar la ultima excepcion
     * capturada, permitiendo propagacion desde handlers catch.
     *
     * @param vm    Proceso virtual que ejecuta RETHROW.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_rethrow(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr; // sin operandos
        do_throw(vm, vm->current_exception); // relanzar la excepcion activa
    }

    // =========================================================================
    //  0xD5 GETCLASS reg_obj -> R0 = ClassInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETCLASS: obtiene el puntero a la clase de un objeto.
     *
     * Lee el ObjectHeader del objeto y devuelve en R00 el puntero host a su ClassInfo.
     * Si el objeto es nulo devuelve 0.
     *
     * @param vm    Proceso virtual que ejecuta GETCLASS.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto).
     */
    void exec_instr_getclass(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();

        if (obj_ptr == 0) {
            vm->registers.regs[R00].qword(0); // objeto nulo -> ClassInfo nula
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(hdr->class_ptr));
    }

    // =========================================================================
    //  0xD6 INSTANCEOF reg_obj, reg_classinfo -> R0 = bool
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion INSTANCEOF: comprueba si un objeto es de una clase.
     *
     * Devuelve 1 en R00 si el objeto es instancia de la clase indicada o de una
     * subclase de ella; 0 en caso contrario o si alguno de los operandos es nulo.
     *
     * @param vm    Proceso virtual que ejecuta INSTANCEOF.
     * @param instr Instruccion descodificada con reg_data.reg1 (obj) y reg_data.reg2 (ClassInfo*).
     */
    void exec_instr_instanceof(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr  = vm->registers.regs[r_obj].qword();
        auto *target_cls  = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0 || target_cls == nullptr) {
            vm->registers.regs[R00].qword(0); // operandos nulos -> falso
            return;
        }

        auto *hdr    = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool result  = is_instance_of(hdr->class_ptr, target_cls); // comprobar jerarquia
        vm->registers.regs[R00].qword(result ? 1ULL : 0ULL);
    }

    // =========================================================================
    //  0xD7 CHECKCAST reg_obj, reg_classinfo -> R0 = obj_ptr o THROW
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CHECKCAST: verifica y proyecta el tipo de un objeto.
     *
     * Si el objeto es compatible con la clase indicada devuelve el puntero en R00.
     * Si no es compatible notifica EVT_ERROR con THREAD_ILLEGAL_INSTRUCTION.
     * El puntero nulo siempre supera la comprobacion (convencion JVM).
     *
     * @param vm    Proceso virtual que ejecuta CHECKCAST.
     * @param instr Instruccion descodificada con reg_data.reg1 (obj) y reg_data.reg2 (ClassInfo*).
     */
    void exec_instr_checkcast(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        auto *target_cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0) {
            // null siempre supera el cast (convencion JVM-compatible)
            vm->registers.regs[R00].qword(0);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool ok   = is_instance_of(hdr->class_ptr, target_cls);

        if (ok) {
            vm->registers.regs[R00].qword(obj_ptr); // cast correcto: devolver el puntero
        } else {
            // tipo incompatible: ClassCastException via FatalError
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CHECKCAST: tipo incompatible (ClassCastException)");
        }
    }

    // =========================================================================
    //  0xD8 GETFIELD reg_classinfo, field_idx -> R0 = FieldInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETFIELD: obtiene el puntero a un descriptor de campo.
     *
     * Devuelve en R00 el puntero host a FieldInfo en el array fields[] de la clase.
     * Devuelve 0 si la clase es nula o el indice esta fuera de rango.
     *
     * @param vm    Proceso virtual que ejecuta GETFIELD.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (field_idx).
     */
    void exec_instr_getfield(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls     = instr.data_instruction.reg_data.reg1; // ClassInfo*
        const uint8_t field_idx = instr.data_instruction.reg_data.reg2; // indice del campo

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || field_idx >= cls->field_count) {
            vm->registers.regs[R00].qword(0); // indice invalido -> puntero nulo
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->fields[field_idx]));
    }

    // =========================================================================
    //  0xD9 GETMETHOD reg_classinfo, method_idx -> R0 = MethodInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETMETHOD: obtiene el puntero a un descriptor de metodo.
     *
     * Devuelve en R00 el puntero host a MethodInfo en el array methods[] de la clase.
     * Devuelve 0 si la clase es nula o el indice esta fuera de rango.
     *
     * @param vm    Proceso virtual que ejecuta GETMETHOD.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (method_idx).
     */
    void exec_instr_getmethod(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls      = instr.data_instruction.reg_data.reg1;
        const uint8_t method_idx = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || method_idx >= cls->method_count) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->methods[method_idx]));
    }

    // =========================================================================
    //  0xDA FIELDCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion FIELDCOUNT: devuelve el numero de campos de una clase.
     *
     * @param vm    Proceso virtual que ejecuta FIELDCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_fieldcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->field_count) : 0ULL);
    }

    // =========================================================================
    //  0xDB METHODCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion METHODCOUNT: devuelve el numero de metodos de una clase.
     *
     * @param vm    Proceso virtual que ejecuta METHODCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_methodcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->method_count) : 0ULL);
    }

    // =========================================================================
    //  0xDC CLASSNAME reg_classinfo -> R0 = char* (host ptr a name.data)
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CLASSNAME: devuelve el nombre de una clase.
     *
     * Escribe en R00 el puntero host al buffer de caracteres del nombre de la clase.
     * Devuelve 0 si la clase es nula o su nombre no tiene datos.
     *
     * @param vm    Proceso virtual que ejecuta CLASSNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classname(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (cls == nullptr || cls->name.data == nullptr) {
            vm->registers.regs[R00].qword(0); // clase sin nombre -> puntero nulo
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(cls->name.data));
    }

    // =========================================================================
    //  Helpers para acceso a doc/attrs
    // =========================================================================

    /**
     * @brief Devuelve el puntero host al buffer de una cadena stringx.
     * @param s Referencia a la cadena stringx.
     * @return Puntero host al buffer de caracteres, o 0 si es nulo.
     */
    static inline uint64_t str_ptr(const loader::stringx &s) {
        return reinterpret_cast<uint64_t>(s.data); // puntero al buffer de la cadena
    }

    /**
     * @brief Devuelve el puntero host a la clave del atributo indicado.
     * @tparam T Tipo que contiene attrs[] y attr_count (ClassInfo, MethodInfo, FieldInfo).
     * @param obj Puntero al objeto con atributos.
     * @param idx Indice del atributo.
     * @return Puntero host a la clave, o 0 si es invalido.
     */
    template<typename T>
    static inline uint64_t attr_key(T *obj, uint8_t idx) {
        if (obj == nullptr || idx >= obj->attr_count || obj->attrs == nullptr)
            return 0; // parametros invalidos
        return reinterpret_cast<uint64_t>(obj->attrs[idx].key.data);
    }

    /**
     * @brief Devuelve el puntero host al valor del atributo indicado.
     * @tparam T Tipo que contiene attrs[] y attr_count (ClassInfo, MethodInfo, FieldInfo).
     * @param obj Puntero al objeto con atributos.
     * @param idx Indice del atributo.
     * @return Puntero host al valor, o 0 si es invalido.
     */
    template<typename T>
    static inline uint64_t attr_val(T *obj, uint8_t idx) {
        if (obj == nullptr || idx >= obj->attr_count || obj->attrs == nullptr)
            return 0; // parametros invalidos
        return reinterpret_cast<uint64_t>(obj->attrs[idx].value.data);
    }

    // =========================================================================
    //  0xDD CLASSDOC -> R0 = char* (host ptr a doc.data)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSDOC: devuelve el puntero al texto de documentacion de la clase.
     * @param vm    Proceso virtual que ejecuta CLASSDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classdoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(cls ? str_ptr(cls->doc) : 0ULL);
    }

    // =========================================================================
    //  0xDE CLASSATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRCOUNT: devuelve el numero de atributos de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xDF CLASSATTRKEY -> R0 = char* (key del atributo)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRKEY: devuelve la clave del atributo indicado de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_classattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(cls, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE0 CLASSATTRVAL -> R0 = char* (valor del atributo)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRVAL: devuelve el valor del atributo indicado de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_classattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(cls, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE1 METHODNAME -> R0 = char* (nombre del metodo)
    // =========================================================================

    /**
     * @brief Ejecuta METHODNAME: devuelve el puntero al nombre de un MethodInfo.
     * @param vm    Proceso virtual que ejecuta METHODNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methodname(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->name) : 0ULL);
    }

    // =========================================================================
    //  0xE2 METHODDOC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODDOC: devuelve el puntero a la documentacion de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methoddoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->doc) : 0ULL);
    }

    // =========================================================================
    //  0xE3 METHODDESC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODDESC: devuelve el puntero al descriptor de tipo de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODDESC.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methoddesc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->descriptor) : 0ULL);
    }

    // =========================================================================
    //  0xE4 METHODATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRCOUNT: devuelve el numero de atributos de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methodattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? static_cast<uint64_t>(m->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xE5 METHODATTRKEY -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRKEY: devuelve la clave del atributo indicado de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_methodattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(m, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE6 METHODATTRVAL -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRVAL: devuelve el valor del atributo indicado de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_methodattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(m, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE7 FIELDNAME -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDNAME: devuelve el puntero al nombre de un FieldInfo.
     * @param vm    Proceso virtual que ejecuta FIELDNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fieldname(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? str_ptr(f->name) : 0ULL);
    }

    // =========================================================================
    //  0xE8 FIELDDOC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDDOC: devuelve el puntero a la documentacion de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fielddoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? str_ptr(f->doc) : 0ULL);
    }

    // =========================================================================
    //  0xE9 FIELDATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRCOUNT: devuelve el numero de atributos de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fieldattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? static_cast<uint64_t>(f->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xEA FIELDATTRKEY -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRKEY: devuelve la clave del atributo indicado de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_fieldattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(f, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xEB FIELDATTRVAL -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRVAL: devuelve el valor del atributo indicado de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_fieldattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(f, instr.data_instruction.reg_data.reg2));
    }

} // namespace runtime
