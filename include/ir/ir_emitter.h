/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_emitter.h
 * @brief Emisor IR -> texto .vel para VestaVM (lowering).
 *
 * Traduce un IrModule (SSA IR) a texto ensamblador .vel que puede
 * compilarse directamente con el pipeline existente:
 *
 *   IrModule -> ir_emit_module() -> texto .vel
 *            -> vm --build  -> archivo .velb
 *            -> vm --run    -> ejecucion en la VM
 *
 * El emisor aplica primero el optimizador IR segun el nivel indicado,
 * calcula los intervalos de vida y asigna registros VM mediante linear
 * scan antes de generar el codigo ensamblador.
 *
 * Convencion de llamada emitida:
 *   - Argumentos: r1, r2, ... r12 (hasta 12 argumentos)
 *   - Contador de argumentos: r15 (argc)
 *   - Valor de retorno: r0
 *   - Frame: enter/leave (guarda rbp, reserva slots de pila para spill)
 *   - r14: scratch del emisor (no asignado por el allocator)
 *
 * Instrucciones de dos direcciones:
 *   .vel usa formato "op dst, src" donde dst tambien es fuente (dst op= src).
 *   El emisor emite un MOV previo si dst != src del operando izquierdo.
 *
 * Spilling:
 *   Los valores que no caben en r0-r13 se derraman (spill) en la pila local.
 *   El emisor usa "push" al definir y "pop" al recargar cada valor derramado.
 *   Los slots de pila se reservan con "enter N" (N = spill_count).
 */

#ifndef IR_EMITTER_H
#define IR_EMITTER_H

#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"
#include <string>

namespace ir {

/**
 * @brief Opciones de emision del emisor IR -> .vel.
 */
struct EmitOptions {
    OptLevel    opt_level      = OptLevel::O1; ///< nivel de optimizacion aplicado antes de emitir
    bool        emit_comments  = true;         ///< emitir comentarios con info de origen IR
    bool        emit_debug     = false;        ///< emitir comentarios @line N para cada instruccion
    bool        export_all     = false;        ///< exportar todas las funciones con @Export
    std::string module_name;                   ///< nombre @Module (vacio = usar mod.name)
};

/**
 * @brief Resultado de la emision de un modulo IR a texto .vel.
 */
struct EmitResult {
    bool        ok;       ///< true si la emision fue exitosa
    std::string vel_text; ///< texto .vel generado (valido si ok)
    std::string error;    ///< mensaje de error si !ok
};

/**
 * @brief Emite un IrModule completo a texto .vel.
 *
 * Pasos internos:
 *   1. ir_optimize(mod, opts.opt_level)
 *   2. Para cada funcion: compute_liveness + allocate_regs
 *   3. Generar prologo de modulo (@Module, @native_lib, @import)
 *   4. Para cada funcion: emitir cuerpo .vel con todos sus bloques
 *
 * @param mod  Modulo IR a emitir (se trabaja sobre una copia interna).
 * @param opts Opciones de emision y nivel de optimizacion.
 * @return Resultado con el texto .vel o el error.
 */
EmitResult ir_emit_module(const IrModule &mod, const EmitOptions &opts = {});

/**
 * @brief Lee un archivo .ir, lo parsea, optimiza y emite texto .vel.
 *
 * Atajo de conveniencia para el flujo completo desde archivo.
 *
 * @param ir_text  Contenido del archivo .ir como cadena.
 * @param opts     Opciones de emision.
 * @return Resultado con el texto .vel o el error.
 */
EmitResult ir_emit_text(const std::string &ir_text, const EmitOptions &opts = {});

} // namespace ir

#endif // IR_EMITTER_H
