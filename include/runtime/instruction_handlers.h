#ifndef INSTRUCTION_HANDLERS_H
#define INSTRUCTION_HANDLERS_H

#include "runtime/instruction_handler.h"

namespace runtime {

// Punteros globales a los handlers (definidos en cada exec_instruction_*.cpp)
extern InstructionHandler * g_core_handler;
extern InstructionHandler * g_alu_handler;
extern InstructionHandler * g_float_handler;
extern InstructionHandler * g_gc_handler;
extern InstructionHandler * g_oop_handler;
extern InstructionHandler * g_string_handler;
extern InstructionHandler * g_closure_handler;
extern InstructionHandler * g_spimm_handler;
extern InstructionHandler * g_pattern_handler;
extern InstructionHandler * g_async_handler;
extern InstructionHandler * g_coro_handler;
extern InstructionHandler * g_sync_handler;
extern InstructionHandler * g_weak_handler;
extern InstructionHandler * g_generic_handler;
extern InstructionHandler * g_distrib_handler;
extern InstructionHandler * g_meta_handler;
extern InstructionHandler * g_exc_handler;

/**
 * @brief Inicializa los handlers de instruccion en las tablas de decode.
 */
void init_instruction_handlers();

} // namespace runtime

#endif // INSTRUCTION_HANDLERS_H
