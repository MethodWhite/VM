/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_cfg.h
 * @brief Reconstruccion del grafo de flujo de control (CFG) de un bloque de
 *        inline asm a partir de sus etiquetas y saltos.
 *
 * Dado el cuerpo textual de un bloque @c asm (sintaxis del ensamblador), separa
 * las instrucciones en BLOQUES BaSICOS y calcula las aristas de control
 * (fallthrough / salto incondicional / rama condicional / retorno).  Es la base
 * del analisis de dataflow del asm (registros/flags como valores), de los
 * diagnosticos (registro sin inicializar, bucle sin salida, codigo muerto) y de
 * los diagramas del CFG en el IDE.
 *
 * La clasificacion del terminador de cada instruccion es POR ISA (x86, arm64,
 * arm32, riscv): que mnemonicos saltan, si es condicional o incondicional, si es
 * un retorno, y cual es la etiqueta destino.  Un salto/llamada INDIRECTO (a
 * registro/memoria) marca el CFG como impreciso (@c has_indirect) porque el
 * destino no se conoce estaticamente.
 *
 * Es 100% propia (no usa Keystone ni Capstone): reutiliza @c asm_canonical_reg /
 * la tabla de efectos por-arquitectura, y su propio troceo de lineas.
 */

#ifndef VEX_ASM_CFG_H
#define VEX_ASM_CFG_H

#include <cstdint>
#include <string>
#include <vector>

#include "vex/asm/instr_db.h" // vex::instr_db::Isa

namespace vex {

/**
 * @brief Clase de terminador de un bloque basico del asm.
 */
enum class AsmTerm : uint8_t {
    Fallthrough, ///< cae a la siguiente instruccion (sin salto).
    UncondJump,  ///< salto incondicional a una etiqueta (jmp / b / j).
    CondBranch,  ///< rama condicional: destino + fallthrough (jCC / b.CC / cbz).
    Call,        ///< llamada: retorna -> fallthrough (call / bl).  No aristas al callee.
    Ret,         ///< retorno: sin sucesores (ret / iret / bx lr).
    Indirect,    ///< salto/llamada a registro/memoria: destino desconocido.
    Unknown      ///< terminador no clasificado (mnemonico de rama desconocido).
};

/**
 * @brief Una instruccion del bloque, con las etiquetas que la preceden.
 */
struct AsmInsn {
    std::string text;                ///< texto de la instruccion (sin las labels).
    std::vector<std::string> labels; ///< etiquetas definidas justo antes.
    AsmTerm term = AsmTerm::Fallthrough; ///< clase de control de flujo.
    std::string target;              ///< etiqueta destino (si UncondJump/CondBranch).
    uint32_t line_no = 0;            ///< indice de linea fisica (para reportes).
};

/**
 * @brief Un bloque basico: rango contiguo de instrucciones con una sola entrada
 *        y una sola salida.
 */
struct AsmBasicBlock {
    uint32_t first = 0;  ///< indice de la primera instruccion (en AsmCfg::insns).
    uint32_t last = 0;   ///< indice de la ultima instruccion (inclusive).
    std::string label;   ///< etiqueta de entrada del bloque ("" si no tiene).
    std::vector<uint32_t> succs; ///< bloques sucesores.
    std::vector<uint32_t> preds; ///< bloques predecesores.
    AsmTerm term = AsmTerm::Fallthrough; ///< terminador del bloque.
};

/**
 * @brief Grafo de flujo de control completo de un bloque de asm.
 */
struct AsmCfg {
    std::vector<AsmInsn> insns;       ///< instrucciones en orden textual.
    std::vector<AsmBasicBlock> blocks;///< bloques basicos.
    std::vector<std::string> unknown_terminators; ///< mnemonicos de rama sin clasificar.
    bool has_indirect = false;        ///< algun salto/llamada indirecto -> CFG impreciso.
    bool has_unresolved_target = false; ///< salto a etiqueta no definida en el bloque.
};

/**
 * @brief Construye el CFG de un bloque de asm.
 *
 * @param isa  ISA del bloque (determina la clasificacion de saltos).
 * @param body Cuerpo textual del bloque @c asm (varias lineas).
 * @return CFG con instrucciones, bloques basicos y aristas.
 */
AsmCfg build_asm_cfg(instr_db::Isa isa, const std::string &body);

/**
 * @brief Clasifica el terminador de una LINEA de asm (mnemonico + operandos).
 *
 * @param isa    ISA del bloque.
 * @param line   Linea sin etiquetas (una instruccion).
 * @param target [out] etiqueta destino si es UncondJump/CondBranch.
 * @return Clase de control de flujo de la instruccion.
 */
AsmTerm asm_classify_term(instr_db::Isa isa, const std::string &line,
                          std::string &target);

} // namespace vex

#endif // VEX_ASM_CFG_H
