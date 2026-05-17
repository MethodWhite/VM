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
 * @file graphviz_diagrams.h
 * @brief Generadores de diagramas Graphviz (DOT) para AST Vex, SSA IR y bytecode .vel.
 *
 * Equivalente al modulo Mermaid (@c mermaid_diagrams.h) pero produciendo el
 * formato DOT que consume Graphviz.  Misma topologia, misma cantidad de
 * informacion -- y en algunos casos MAS, ya que DOT acepta atributos
 * arbitrarios (tooltip, URL, fontname, fontsize, fillcolor, color,
 * penwidth, style, shape, rankdir...) sin las restricciones sintacticas
 * de Mermaid.
 *
 * Diferencias visuales / informativas frente a Mermaid:
 *   - Cada nodo lleva atributos `tooltip` con info extra (line numbers,
 *     conteos, tipos completos) que aparecen al hover en herramientas
 *     interactivas (Graphviz Online, xdot, Gephi via DOT export).
 *   - Subgraphs son `cluster_*` con titulo y bordes propios; permiten
 *     anidar (cluster_module > cluster_function > cluster_body) y son
 *     visualmente mas claros que los `subgraph` planos de Mermaid.
 *   - Edges con peso (weight) controlable y rank constraints (samerank,
 *     min, max) para layouts mas legibles en grafos grandes.
 *   - Forma de los nodos por tipo: box, ellipse, diamond, doublecircle,
 *     octagon -- cubre la semantica del nodo (entry/exit/branch/loop) sin
 *     depender solo del color.
 *   - Salida lista para `dot -Tpng salida.dot -o salida.png`,
 *     `dot -Tsvg ...` o cualquier herramienta compatible con DOT.
 *
 * Usar siempre que el output Mermaid se quede corto: PRs con grafos grandes
 * (mas de 200 nodos) que Mermaid renderiza lento, papers/diapositivas que
 * requieren control fino del layout, o exportar a PDF/SVG sin pasar por
 * el render web de Mermaid.
 *
 * Pipeline tipico:
 *   ./vm --vex foo.vex --diagram-all --diagram-format=graphviz -o foo
 *   dot -Tsvg foo.ast.dot   -o foo.ast.svg
 *   dot -Tpng foo.ir.pre.dot -o foo.ir.pre.png
 *   dot -Tsvg foo.vel.dot    -o foo.vel.svg
 */

#ifndef VEX_GRAPHVIZ_DIAGRAMS_H
#define VEX_GRAPHVIZ_DIAGRAMS_H

#include <string>

namespace ast { struct ModuleNode; }
namespace ir  { struct IrModule;  }

namespace vex {

    /**
     * @brief Genera un diagrama Graphviz (DOT) a partir del AST Vex post type-check.
     *
     * Estructura del diagrama:
     *   - digraph G top-level con rankdir=TB.
     *   - Cluster "module" raiz con conteo de declaraciones y filename.
     *   - Por cada FunctionDecl: cluster con info de la funcion + cluster
     *     anidado del body con todos los stmts.
     *   - Por cada ClassDecl: cluster con super, interfaces, fields, methods;
     *     cada metodo con su cluster body.
     *   - Por cada StructDecl/EnumDecl: cluster con fields/variants.
     *   - Por cada GlobalVarDecl/ExternFnDecl: nodo simple con tipo+nombre.
     *
     * Adicional sobre Mermaid:
     *   - Tooltips por nodo con detalle expandido (depth=4 vs depth=3).
     *   - URL/href en nodos para enlazar a `file:line`.
     *   - Fontsize y fontname controlados explicitamente.
     *   - Edges con tipo (dotted/solid/dashed) y label posicional (head/tail/mid).
     *
     * @param mod    AST modulo (post type-check).
     * @return       String con el codigo DOT completo (digraph G { ... }).
     *               NO lleva markdown wrapper; volcarlo a fichero .dot directo.
     */
    std::string graphviz_from_ast(const ast::ModuleNode &mod);

    /**
     * @brief Genera un diagrama Graphviz (DOT) del SSA IR.
     *
     * Estructura:
     *   - digraph G con rankdir=TB.
     *   - Por cada IrFunction: cluster con sus IrBlocks.
     *   - Cada IrBlock = nodo `record` con:
     *       cabecera: nombre + #id
     *       lineas: hasta MAX_INSTRS_SHOWN instrucciones (igual que Mermaid)
     *       nota: total restante.
     *   - Edges:
     *       BR plano: solid.
     *       BR_COND: dos edges con label "true"/"false" (tail/head ports).
     *       CALL/CALLN: dotted cross-cluster cuando el callee es local.
     *
     * Adicional sobre Mermaid:
     *   - Nodos `record` permiten resaltar un instr individual via puerto.
     *   - rank=same forzado para PHIs en el mismo bloque (mas legible).
     *   - Tooltips por bloque con conteo total de instrs y tipo terminador.
     *
     * @param mod          IrModule (pre o post optimizacion).
     * @param title        Titulo (e.g. "SSA IR pre-opt"), va al `label` del digraph.
     * @return             String con el codigo DOT.
     */
    std::string graphviz_from_ir_module(const ir::IrModule &mod,
                                         const std::string &title);

    /**
     * @brief Genera un diagrama Graphviz (DOT) del bytecode .vel ensamblador.
     *
     * Estructura:
     *   - digraph G con rankdir=TB.
     *   - Por cada label encontrado en el .vel: nodo `record` con instr lines.
     *   - Edges segun los saltos detectados (jmp/jmp.cc/cmpjmp/decjnz/callvm).
     *
     * Adicional sobre Mermaid:
     *   - Opcodes optimizados (cmpjmp/cmpjmpu/decjnz/gcallocp/spawnargs/
     *     fulfillhlt) con bordes destacados Y fillcolor distintivo.
     *   - Edges call vs branch tienen `style` y `color` diferenciados.
     *   - Tooltip por nodo lista TODAS las instrucciones (no solo las
     *     primeras 10), util para bloques grandes.
     *
     * @param vel_text   Codigo .vel completo.
     * @return           String con el codigo DOT.
     */
    std::string graphviz_from_vel_text(const std::string &vel_text);

} // namespace vex

#endif // VEX_GRAPHVIZ_DIAGRAMS_H
