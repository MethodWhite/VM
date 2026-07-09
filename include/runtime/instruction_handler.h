#ifndef INSTRUCTION_HANDLER_H
#define INSTRUCTION_HANDLER_H

#include "runtime/decode_instruction.h"

namespace runtime {

/**
 * @brief Interface virtual para familias de instrucciones.
 *
 * Cada familia (ALU, Float, GC, String, OOP, etc.) implementa esta interfaz
 * y registra sus opcodes en la tabla de decode.  Esto permite:
 *   - Testing individual por familia sin levantar el VM completo
 *   - Plugins de instrucciones nuevos sin modificar el core
 *   - WASM/JIT/backends alternativos registrando sus propios handlers
 *   - Cada familia compilable como TU separada
 */
class InstructionHandler {
public:
    virtual ~InstructionHandler() = default;

    /**
     * @brief Nombre descriptivo de la familia (para debugging).
     */
    virtual const char* name() const = 0;

    /**
     * @brief Ejecuta una instruccion de esta familia.
     *
     * @param vm    Proceso virtual sobre el que se ejecuta.
     * @param instr Instruccion descodificada con todos sus operandos.
     */
    virtual vm_event execute(ProcessVM *vm, const DecodedInstr &instr) = 0;
};

/**
 * @brief Wrapper que adapta una funcion libre existente al interface
 *        InstructionHandler.  Permite la migracion gradual: cada
 *        funcion exec_instr_* existente se envuelve en un WrapperHandler
 *        sin modificar su implementacion.
 *
 * En la Fase 2 cada familia creara su propio handler con dispatch
 * interno, eliminando la necesidad de un wrapper por opcode.
 */
class WrapperHandler : public InstructionHandler {
public:
    using ExecFn = void (*)(ProcessVM *, const DecodedInstr &);

    constexpr WrapperHandler(const char *name, ExecFn fn)
        : name_(name), fn_(fn) {}

    const char* name() const override { return name_; }

    vm_event execute(ProcessVM *vm, const DecodedInstr &instr) override {
        if (fn_) fn_(vm, instr);
        return EVT_EXEC_DONE;
    }

private:
    const char *name_;
    ExecFn      fn_;
};

} // namespace runtime

#endif // INSTRUCTION_HANDLER_H
