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
 * @file exec_instruction_exc.cpp
 * @brief Implementacion de las instrucciones TRYENTER y TRYLEAVE de VestaVM.
 *
 * TRYENTER apila un ExceptionFrame ligero en ProcessVM::exc_frame_stack.
 * TRYLEAVE desapila el frame del tope.  do_throw (exec_instruction_oop.cpp)
 * comprueba esta pila antes de recorrer MethodInfo.handlers, lo que permite
 * a compiladores de alto nivel instalar handlers dinamicos sin anotaciones
 * en el bytecode.
 *
 * Encoding:
 *   TRYENTER FIXED_4 REG: [0x00][0x44][ctrl][byte3]
 *     ctrl  = (r_handler<<4)|r_type   (r_handler=PC handler, r_type=ClassInfo* o 0)
 *     byte3 = reservado (0x00)
 *   TRYLEAVE FIXED_2:     [0x00][0x45]
 */
#include "runtime/exec_instruction.h"

namespace runtime {

    /**
     * @brief Ejecuta TRYENTER r_handler, r_type: instala un frame de excepcion dinamico.
     *
     * Apila un nuevo ExceptionFrame en exc_frame_stack del proceso con:
     *   - handler_pc = valor del registro r_handler (direccion absoluta VM)
     *   - type       = valor del registro r_type interpretado como ClassInfo*
     *                  (0 = catch-all, captura cualquier excepcion)
     *
     * El frame se destruye cuando TRYLEAVE lo desapila o cuando do_throw lo consume.
     *
     * @param vm    Proceso virtual que ejecuta la instruccion.
     * @param instr Instruccion descodificada; reg1=r_handler, reg2=r_type.
     */
    void exec_instr_tryenter(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handler = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF; // nibble alto de byte2
        const uint8_t r_type    =  instr.data_instruction.reg_data.reg1        & 0xF; // nibble bajo de byte2

        uint64_t handler_pc = vm->registers.regs[r_handler].qword();   // direccion absoluta del handler
        auto *class_ptr = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[r_type].qword());                        // tipo capturado (puede ser nullptr)

        auto *ef       = new ProcessVM::ExceptionFrame();               // alocar nuevo frame
        ef->handler_pc = handler_pc;                                    // guardar PC del handler
        ef->type       = class_ptr;                                     // guardar tipo capturado
        // snapshot RSP/RBP/frame_stack al momento del tryenter.
        // do_throw los restaura para descartar pushes del regalloc y
        // frames de calls anidados que no retornaron normalmente por el
        // throw.  Sin esto, los registros vivos guardados en stack quedan
        // corruptos despues del catch (caso visto: callvirt en obj null
        // dentro del try, push r1 sin pop -> r1 corrupto en el merge).
        ef->saved_rsp         = vm->registers.stack_pointer.qword();
        ef->saved_rbp         = vm->registers.base_pointer.qword();
        ef->saved_frame_stack = (uint64_t)(uintptr_t)vm->frame_stack;
        ef->prev       = vm->exc_frame_stack;                           // encadenar con el frame anterior
        vm->exc_frame_stack = ef;                                       // empujar al tope de la pila
    }

    /**
     * @brief Ejecuta TRYLEAVE: desinstala el frame de excepcion del tope de exc_frame_stack.
     *
     * Se debe llamar al salir del bloque try de forma normal (sin excepcion).
     * Si la pila esta vacia la instruccion es un no-op silencioso.
     *
     * @param vm    Proceso virtual que ejecuta la instruccion.
     * @param instr Instruccion descodificada (sin operandos).
     */
    void exec_instr_tryleave(ProcessVM *vm, const DecodedInstr &/*instr*/) {
        if (vm->exc_frame_stack == nullptr) return; // pila vacia: no-op silencioso

        ProcessVM::ExceptionFrame *top = vm->exc_frame_stack; // frame del tope
        vm->exc_frame_stack = top->prev;                      // desapilar
        delete top;                                           // liberar la memoria
    }

} // namespace runtime
