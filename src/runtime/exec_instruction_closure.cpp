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
 * @file exec_instruction_closure.cpp
 * @brief Implementacion de las instrucciones de closures de VestaVM.
 *
 * Implementa los dos modelos de closure:
 *
 *  Modelo A - Closure GC (mkclosure / callclosure, opcodes 0x20-0x21):
 *    El closure se almacena como un ClosureObject en el heap GC.
 *    El GC escanea las capturas durante la fase de marcado.
 *    Adecuado para codigo interno de la VM y codigo OOP.
 *
 *  Modelo B - Closure raw (mkrawclosure / callrawclosure, opcodes 0x22-0x23):
 *    El closure se almacena como un RawClosureObject sin GC.
 *    Adecuado para funciones nativas (FFI) donde el ciclo de vida es manual.
 *
 * Ademas implementa:
 *  - TAILCALL (0x24): llamada en posicion de cola sin crecimiento de pila.
 *  - ISNULL   (0x25): comprobacion de nulidad sin excepcion.
 *  - UNWRAP   (0x26): desenvuelve un nullable o lanza NullPointerException.
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/exception_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

namespace runtime {

    // =========================================================================
    //  0x20  MKCLOSURE  r_method, r_env
    // =========================================================================

    /**
     * @brief Ejecuta MKCLOSURE: crea un ClosureObject gestionado por el GC.
     *
     * Operandos:
     *   reg1 = puntero host (uint64_t) a un MethodInfo del lambda capturado.
     *   reg2 = direccion VM del bloque de entorno (variables capturadas).
     *
     * El nuevo ClosureObject se aloca en el heap GC. Su header apunta a una
     * ClassInfo con CLASS_FLAG_CLOSURE. R0 recibe el GcHandle del objeto.
     * Si la alocation falla R0 = GC_NULL_HANDLE.
     *
     * @param vm    Proceso virtual que ejecuta MKCLOSURE.
     * @param instr reg1 = MethodInfo*, reg2 = env addr.
     */
    void exec_instr_mkclosure(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_method = instr.data_instruction.reg_data.reg1; // registro con MethodInfo*
        const uint8_t r_env    = instr.data_instruction.reg_data.reg2; // registro con env addr

        auto *method   = reinterpret_cast<loader::MethodInfo *>(vm->registers.regs[r_method].qword());
        uint64_t env_addr = vm->registers.regs[r_env].qword(); // direccion VM del entorno

        // alocar el ClosureObject en el heap GC
        gc::GcHandle h = vm->gc_heap.alloc(sizeof(loader::ClosureObject));
        if (h == gc::GC_NULL_HANDLE) {
            vm->registers.regs[0].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return; // alocation fallida; el llamante debe comprobar el handle
        }

        uint8_t *payload = vm->gc_heap.deref(h); // puntero host al payload del objeto
        if (payload == nullptr) {
            vm->registers.regs[0].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return;
        }

        auto *clos        = reinterpret_cast<loader::ClosureObject *>(payload);
        clos->header.flags     = loader::OBJ_FLAG_GC_OWNED;  // objeto gestionado por GC
        clos->header.hash_code = static_cast<uint32_t>(h);   // hash de identidad = handle
        clos->header.class_ptr = nullptr;                     // el loader/JIT puede asignar ClassInfo
        clos->method      = method;
        clos->captures    = nullptr;  // se rellena mas tarde si hay capturas
        clos->cap_count   = 0;

        // guardar la direccion del bloque de entorno en el campo captures como opaco
        // (el frontend usa env_addr como puntero a un array de GcHandle en memoria VM)
        clos->captures = reinterpret_cast<uint32_t *>(env_addr);

        vm->registers.regs[0].qword(static_cast<uint64_t>(h)); // retornar el handle en R0
    }

    // =========================================================================
    //  0x21  CALLCLOSURE  r_closure
    // =========================================================================

    /**
     * @brief Ejecuta CALLCLOSURE: invoca un ClosureObject GC.
     *
     * Desreferencia el GcHandle en reg1 para obtener el ClosureObject.
     * Empuja la direccion de retorno en la pila convencional y crea un
     * FrameHeader con el MethodInfo del closure para soportar throw/catch.
     * Finalmente salta a method->code_vaddr.
     *
     * @param vm    Proceso virtual que ejecuta CALLCLOSURE.
     * @param instr reg1 = GcHandle del ClosureObject.
     */
    void exec_instr_callclosure(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_clos = instr.data_instruction.reg_data.reg1;
        const uint64_t raw_h  = vm->registers.regs[r_clos].qword(); // valor del GcHandle

        if (raw_h == static_cast<uint64_t>(gc::GC_NULL_HANDLE)) {
            runtime::throw_fatal(vm, runtime::FATAL_NULL_POINTER,
                "CALLCLOSURE: GcHandle nulo");
            vm->decoded_ptr->flags_info.blocking = true;
            return;
        }

        uint8_t *payload = vm->gc_heap.deref(static_cast<gc::GcHandle>(raw_h));
        if (payload == nullptr) {
            runtime::throw_fatalf(vm, runtime::FATAL_NULL_POINTER,
                "CALLCLOSURE: GcHandle invalido (raw=0x%llX)",
                (unsigned long long)raw_h);
            vm->decoded_ptr->flags_info.blocking = true;
            return;
        }

        auto *clos = reinterpret_cast<loader::ClosureObject *>(payload);
        loader::MethodInfo *method = clos->method;

        if (method == nullptr || method->code_vaddr == 0) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CALLCLOSURE: closure sin cuerpo (method=null o code_vaddr=0)");
            vm->decoded_ptr->flags_info.blocking = true;
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr); // PC post-instruccion

        // empujar FrameHeader para soporte de excepciones
        auto *frame       = vm->frame_pool.acquire(); // A.34.fix13
        frame->prev       = vm->frame_stack;
        frame->method     = method;
        frame->return_pc  = ret_addr;
        frame->frame_base = vm->registers.stack_pointer.qword();
        frame->proceed_target = nullptr;
        vm->frame_stack   = frame;

        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        // Mantener write para callvm anidado.
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        vm->registers.rip.qword(method->code_vaddr); // saltar al lambda
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0x22  MKRAWCLOSURE  r_fn, r_env
    // =========================================================================

    /**
     * @brief Ejecuta MKRAWCLOSURE: crea un RawClosureObject sin GC.
     *
     * Aloca un RawClosureObject via RawAllocator y lo inicializa con:
     *   fn_addr  = reg1 (direccion de la funcion nativa o bytecode)
     *   env_addr = reg2 (puntero al bloque de entorno en memoria VM)
     *   env_size = 0    (el caller debe rellenarlo si es necesario)
     *
     * R0 recibe el puntero raw al objeto (0 si falla la alocation).
     *
     * @param vm    Proceso virtual que ejecuta MKRAWCLOSURE.
     * @param instr reg1 = fn_addr, reg2 = env_addr.
     */
    void exec_instr_mkrawclosure(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_fn  = instr.data_instruction.reg_data.reg1;
        const uint8_t r_env = instr.data_instruction.reg_data.reg2;

        uint64_t fn_addr  = vm->registers.regs[r_fn].qword();
        uint64_t env_addr = vm->registers.regs[r_env].qword();

        // alocar con el raw allocator (sin GC; ciclo de vida manual)
        uint64_t ptr = vm->raw_alloc.alloc(sizeof(loader::RawClosureObject));
        if (ptr == 0) {
            vm->registers.regs[0].qword(0); // fallo de alocation
            return;
        }

        auto *rc    = reinterpret_cast<loader::RawClosureObject *>(ptr);
        rc->fn_addr  = fn_addr;
        rc->env_addr = env_addr;
        rc->env_size = 0; // el caller ajusta si necesita saber el tamano del entorno

        vm->registers.regs[0].qword(ptr); // retornar puntero raw en R0
    }

    // =========================================================================
    //  0x23  CALLRAWCLOSURE  r_closure
    // =========================================================================

    /**
     * @brief Ejecuta CALLRAWCLOSURE: invoca la funcion nativa de un RawClosureObject.
     *
     * Lee el puntero RawClosureObject de reg1, extrae fn_addr y lo invoca
     * siguiendo la convencion calln: r1-r12 argumentos, r0 retorno, r15 argc.
     * El bloque de entorno (env_addr) se pasa en R14 para que la funcion
     * nativa pueda acceder a las capturas.
     *
     * @param vm    Proceso virtual que ejecuta CALLRAWCLOSURE.
     * @param instr reg1 = puntero raw al RawClosureObject.
     */
    void exec_instr_callrawclosure(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_clos = instr.data_instruction.reg_data.reg1;
        const uint64_t ptr    = vm->registers.regs[r_clos].qword(); // puntero raw

        if (ptr == 0) {
            runtime::throw_fatal(vm, runtime::FATAL_NULL_POINTER,
                "CALLRAWCLOSURE: puntero nulo");
            vm->decoded_ptr->flags_info.blocking = true;
            return;
        }

        auto *rc = reinterpret_cast<loader::RawClosureObject *>(ptr);

        if (rc->fn_addr == 0) {
            runtime::throw_fatal(vm, runtime::FATAL_ILLEGAL_INSTRUCTION,
                "CALLRAWCLOSURE: fn_addr nulo");
            vm->decoded_ptr->flags_info.blocking = true;
            return;
        }

        // pasar el env_addr en R14 (convencion de closure raw)
        // R1-R12 ya contienen los argumentos establecidos por el llamante
        vm->registers.regs[14].qword(rc->env_addr);

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr);

        // empujar la direccion de retorno en la pila convencional
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        write_rip(vm, rc->fn_addr); // saltar a la funcion nativa/bytecode
    }

    // =========================================================================
    //  0x24  TAILCALL  r_fn
    // =========================================================================

    /**
     * @brief Ejecuta TAILCALL: llamada en posicion de cola sin crecimiento de pila.
     *
     * Si existe un FrameHeader activo, restaura SP al frame_base (deshaciendo
     * la reserva del frame actual) y lo desapila sin crear uno nuevo.
     * A continuacion salta a fn_addr sin empujar una nueva direccion de retorno;
     * la direccion de retorno del frame padre ya esta en la pila.
     *
     * Esto permite recursion mutua o directa sin consumir pila adicional.
     *
     * @param vm    Proceso virtual que ejecuta TAILCALL.
     * @param instr reg1 = direccion de la funcion destino.
     */
    void exec_instr_tailcall(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_fn   = instr.data_instruction.reg_data.reg1;
        const uint64_t fn_addr = vm->registers.regs[r_fn].qword();

        if (vm->frame_stack != nullptr) {
            loader::FrameHeader *frame = vm->frame_stack;
            // restaurar SP al punto de entrada del frame actual para liberar su espacio
            vm->registers.stack_pointer.qword(frame->frame_base);
            vm->frame_stack = frame->prev; // desapilar el frame
            vm->frame_pool.release(frame); // fix13
        }

        // no se empuja nueva direccion de retorno: la del llamante ya esta en la pila
        write_rip(vm, fn_addr); // saltar a la funcion destino
    }

    // =========================================================================
    //  0x25  ISNULL  r_dst, r_src
    // =========================================================================

    /**
     * @brief Ejecuta ISNULL: comprueba si un registro es nulo sin lanzar excepcion.
     *
     * r_dst = (r_src == 0) ? 1 : 0
     * No modifica los flags de la VM.
     *
     * @param vm    Proceso virtual que ejecuta ISNULL.
     * @param instr reg1 = destino, reg2 = valor a comprobar.
     */
    void exec_instr_isnull(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_dst = instr.data_instruction.reg_data.reg1;
        const uint8_t  r_src = instr.data_instruction.reg_data.reg2;
        const uint64_t val   = vm->registers.regs[r_src].qword();
        vm->registers.regs[r_dst].qword(val == 0 ? 1ULL : 0ULL);
    }

    // =========================================================================
    //  0x26  UNWRAP  r_dst, r_src
    // =========================================================================

    /**
     * @brief Ejecuta UNWRAP: desenvuelve un valor nullable o lanza NullPointerException.
     *
     * Si r_src != 0 -> r_dst = r_src (valor desenvuelto).
     * Si r_src == 0 -> lanza NullPointerException mediante el mecanismo do_throw.
     *
     * La excepcion se construye como un ObjectHeader plano con class_ptr = nullptr
     * y flags = 0. El loader deberia registrar una ClassInfo de NullPointerException
     * para que las clausulas catch puedan capturarla por tipo.
     *
     * @param vm    Proceso virtual que ejecuta UNWRAP.
     * @param instr reg1 = destino, reg2 = valor nullable.
     */
    void exec_instr_unwrap(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_dst = instr.data_instruction.reg_data.reg1;
        const uint8_t  r_src = instr.data_instruction.reg_data.reg2;
        const uint64_t val   = vm->registers.regs[r_src].qword();

        if (val == 0) {
            // Valor nulo: lanzar NullPointerException via el mismo
            // mecanismo que la instruccion `throw`, para que sea
            // capturable por try/catch.  Si no hay handler en
            // exc_frame_stack, el throw escala a error fatal del thread
            // (THREAD_NULL_POINTER).  Esto unifica el modelo: unwrap
            // failure y throw user-level se procesan por el mismo
            // unwinder.
            uint64_t ex_ptr = vm->raw_alloc.alloc(sizeof(loader::ObjectHeader));
            if (ex_ptr != 0) {
                auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(ex_ptr);
                hdr->class_ptr = nullptr;                    // excepcion sin ClassInfo (catch-all)
                hdr->flags     = loader::OBJ_FLAG_RAW_OWNED;
                hdr->hash_code = 0;
            }
            // Forward declaration: do_throw vive en exec_instruction_oop.cpp.
            extern void do_throw(ProcessVM *vm, uint64_t exception_ptr);
            do_throw(vm, ex_ptr);
            return;
        }

        vm->registers.regs[r_dst].qword(val); // desenvolver el valor
    }

} // namespace runtime
