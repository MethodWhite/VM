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
 * @file mermaid_diagrams.h
 * @brief Generadores de diagramas Mermaid para AST Vex, SSA IR y bytecode .vel.
 *
 * Pensado para debug, traceo y analisis post-mortem.  El output es markdown
 * con un bloque @c mermaid envolvente, listo para abrir en VS Code (con la
 * extension Markdown Preview Mermaid Support), GitHub, GitLab o herramientas
 * online (mermaid.live).
 *
 * Cuatro vistas complementarias del programa cubren el pipeline completo:
 *
 *   1. mermaid_from_ast(): forma del codigo fuente Vex (post type-check).
 *      Util para ver la estructura logica: jerarquia clase/metodo, herencia,
 *      anotaciones (@Async, @Override, @Aspect), parametros y returns.
 *
 *   2. mermaid_from_ir_module(): SSA IR (pre o post optimizacion).
 *      Util para ver el control flow basic block, donde estan los PHIs,
 *      como se conectan los blocks via BR/BR_COND, y diferenciar pre-opt
 *      vs post-opt para entender que hizo el optimizer (DCE, const fold,
 *      TCO, inline_loop_header).
 *
 *   3. mermaid_from_vel_text(): bytecode .vel ensamblador final.
 *      Util para ver el output final: labels reales, opcodes emitidos
 *      (incluyendo opt como cmpjmp.cc, decjnz, gcallocp), y los saltos
 *      entre labels.  Lo que ejecuta la VM, sin abstracciones.
 *
 * Cada uno emite una vista complementaria del MISMO programa.  Usar los
 * cuatro juntos durante una sesion de debug permite trazar como una
 * funcion va de codigo Vex -> AST -> IR -> IR optimizado -> .vel sin
 * cambiar de herramienta.
 *
 * Filosofia de informacion:
 *   - Cada nodo lleva todo lo que es relevante para diagnostico:
 *       AST: kind del nodo, nombre, modificadores, tipos, num stmts.
 *       IR:  block id, instrs (op + dst + tipo), terminador, source_line.
 *       VEL: label, opcodes con operandos, target de saltos.
 *   - Edges colorean por significado: control flow, llamada, herencia,
 *     interface, exception handler.
 *   - Los strings se sanitizan para que cuadren en Mermaid (escape de
 *     `<`, `>`, `"`, saltos de linea -> `<br/>`).
 */

#ifndef VEX_MERMAID_DIAGRAMS_H
#define VEX_MERMAID_DIAGRAMS_H

#include <string>

namespace ast { struct ModuleNode; }
namespace ir  { struct IrModule;  }

namespace vex {

    /**
     * @brief Genera un diagrama Mermaid a partir del AST Vex post type-check.
     *
     * Estructura del diagrama (flowchart TD):
     *   - Nodo raiz: el modulo (con nombre y conteo de declaraciones).
     *   - Por cada FunctionDecl: nodo de funcion con anotacion @Async,
     *     return type, parametros (cada uno como subnodo con tipo).
     *   - Por cada ClassDecl: nodo de clase con super (edge "extends"),
     *     interfaces (edges "implements"), fields y methods.
     *   - Por cada StructDecl: nodo con fields.
     *   - Por cada EnumDecl: nodo con variantes.
     *   - Por cada GlobalVarDecl: nodo con tipo y nombre.
     *
     * Los stmts internos del cuerpo de cada funcion NO se expanden por
     * defecto (solo se muestra `BlockStmt: N stmts`).  Para el detalle
     * de control flow del cuerpo usar @c mermaid_from_ir_module.
     *
     * @param mod    AST modulo (post type-check, con result_type rellenos).
     * @return       String con el bloque ```mermaid ... ``` listo para
     *               escribir a archivo .md o .mmd.
     */
    std::string mermaid_from_ast(const ast::ModuleNode &mod);

    /**
     * @brief Genera un diagrama Mermaid del SSA IR de un IrModule.
     *
     * Estructura del diagrama (flowchart TD):
     *   - Por cada IrFunction: subgraph con sus IrBlocks.
     *   - Cada IrBlock = nodo cuyo label muestra:
     *       block_name:
     *       %dst1 = op operand1, operand2  (line N)
     *       %dst2 = op ...
     *       terminator (BR/BR_COND/RET/HLT)
     *   - Edges:
     *       BR plano: solid arrow al target_block.
     *       BR_COND: dos edges, label "true" al target_block, "false" al false_block.
     *       CALL/CALLN/CALLVIRT/CALLM/SPAWN/RSPAWN: edge punteado entre funciones
     *       cuando el callee es del mismo modulo (analisis de callgraph local).
     *
     * Las instrucciones se truncan a 3-4 por bloque por legibilidad;
     * bloques mas grandes se anotan con `... (N instrs total)`.  El
     * detalle exhaustivo esta disponible via el dump textual del IR
     * (--vex-emit-ir) que conserva todas las instrucciones.
     *
     * @param mod          IrModule a graficar (pre o post optimizacion).
     * @param title        Titulo del diagrama, incluido en un comentario
     *                     superior (e.g., "SSA IR pre-optimizacion" o
     *                     "SSA IR post-optimizacion (opt_level=2)").
     * @return             String con el bloque mermaid listo para escribir.
     */
    std::string mermaid_from_ir_module(const ir::IrModule &mod,
                                        const std::string &title);

    /**
     * @brief Genera un diagrama Mermaid a partir del texto .vel ensamblador.
     *
     * El texto .vel es lo que produce el frontend Vex tras lowering+emit.
     * El parser interno de este generador es simple: detecta lineas con
     * @c label: para delimitar bloques, y dentro de cada bloque captura
     * las instrucciones hasta el siguiente label o EOF.
     *
     * Estructura del diagrama:
     *   - Por cada label encontrado: nodo cuyo label Mermaid muestra:
     *       label_name:
     *       opcode1 operand1, operand2
     *       opcode2 ...
     *       terminator (jmp/jmp.cc/ret/hlt/callvm)
     *   - Edges:
     *       jmp @Absolute("code.X"): edge solid al label X.
     *       jmp.cc @Absolute("code.X"): edge solid con label cc al label X.
     *       cmpjmp.cc r,r,@Absolute("code.X"): edge solid label cc al label X.
     *       callvm @Absolute("code.X"): edge dotted (call) al label X.
     *       ret/hlt: nodo terminal sin edge saliente.
     *
     * Tambien identifica:
     *   - Anotaciones `@Module`, `@Section`, `@Export` (mostradas como
     *     comentario en el diagrama).
     *   - Instrucciones especiales nuevas (cmpjmp, cmpjmpu, decjnz,
     *     gcallocp, spawnargs, fulfillhlt) destacadas con color/estilo.
     *
     * @param vel_text   Codigo .vel completo como string.
     * @return           String con el bloque mermaid listo para escribir.
     */
    std::string mermaid_from_vel_text(const std::string &vel_text);

} // namespace vex

#endif // VEX_MERMAID_DIAGRAMS_H
