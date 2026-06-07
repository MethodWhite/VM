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
 * @file html_diagrams.h
 * @brief Generadores de diagramas HTML+CSS+JS interactivos para AST/IR/.vel.
 *
 * Tercer formato de salida del pipeline de diagramas, complementario a
 * Mermaid (@c mermaid_diagrams.h) y Graphviz (@c graphviz_diagrams.h).  En
 * lugar de un artefacto estatico (.mmd / .dot), produce una pagina HTML
 * AUTOCONTENIDA (CSS + JS embebidos, cero dependencias externas ni CDN) que
 * el usuario abre en cualquier navegador para analizar el flujo de codigo
 * de forma interactiva:
 *
 *   - Pan (arrastrar) + zoom (rueda / botones) + ajustar-a-pantalla.
 *   - Click en un nodo -> panel lateral con TODA su informacion (cabecera,
 *     instrucciones completas, tipo, grupo, linea fuente, aristas in/out).
 *   - Busqueda incremental que resalta nodos y atenua el resto.
 *   - Leyenda de aristas con checkboxes para mostrar/ocultar por tipo
 *     (control flow, true/false, call, herencia, exception, back-edge).
 *   - Modo compacto (solo cabeceras) para grafos grandes + foco por funcion.
 *   - Cajas de grupo (una por funcion / cuerpo) con titulo.
 *
 * Estrategia de implementacion (paridad garantizada, cero divergencia):
 *   la entrada NO es el AST/IR/.vel directamente, sino el DOT que ya
 *   producen los generadores Graphviz.  @c html_from_dot parsea ese DOT a
 *   un modelo de grafo (grupos + nodos + aristas con sus atributos) y lo
 *   embebe como JSON dentro de la plantilla HTML.  Asi el HTML hereda
 *   AUTOMATICAMENTE toda la cobertura de los generadores Graphviz (las
 *   cuatro vistas: AST, IR pre, IR post, .vel) y cualquier mejora futura
 *   de la traversal fluye a los tres formatos sin tocar este modulo.
 *
 * Filosofia de informacion: el HTML lleva TANTA o MAS info que Mermaid/DOT.
 *   El DOT ya empaqueta tooltips con detalle expandido (line numbers,
 *   conteos, terminador); el HTML los muestra siempre en el panel de
 *   detalle, sin depender del hover de una herramienta externa.
 */

#ifndef VEX_HTML_DIAGRAMS_H
#define VEX_HTML_DIAGRAMS_H

#include <string>

namespace ast { struct ModuleNode; }
namespace ir  { struct IrModule;  }

namespace vex {

    /**
     * @brief Convierte un diagrama Graphviz (DOT) en una pagina HTML interactiva.
     *
     * Parsea el DOT (clusters, nodos record/box/diamond/..., aristas con
     * label/color/style) a un modelo de grafo y lo renderiza dentro de una
     * plantilla HTML autocontenida con el motor de layout + interaccion en
     * JS embebido.
     *
     * @param dot_source   Texto DOT completo (digraph G { ... }) tal cual lo
     *                     produce @c graphviz_from_ast/ir_module/vel_text.
     * @param title        Titulo mostrado en la cabecera de la pagina.
     * @param view_kind    Identificador corto de la vista ("ast", "ir-pre",
     *                     "ir-post", "vel"); afecta solo a etiquetas de UI.
     * @return             Documento HTML completo (`<!DOCTYPE html> ...`).
     */
    std::string html_from_dot(const std::string &dot_source,
                              const std::string &title,
                              const std::string &view_kind);

    /**
     * @brief HTML interactivo del AST Vex post type-check.
     *
     * Conveniencia simetrica a @c mermaid_from_ast / @c graphviz_from_ast:
     * genera el DOT del AST internamente y lo pasa a @c html_from_dot.
     *
     * @param mod  AST modulo (post type-check).
     * @return     Documento HTML completo.
     */
    std::string html_from_ast(const ast::ModuleNode &mod);

    /**
     * @brief HTML interactivo del SSA IR (pre o post optimizacion).
     *
     * @param mod    IrModule a graficar.
     * @param title  Titulo (e.g. "SSA IR pre-optimizacion").
     * @return       Documento HTML completo.
     */
    std::string html_from_ir_module(const ir::IrModule &mod,
                                    const std::string &title);

    /**
     * @brief HTML interactivo del bytecode .vel ensamblador final.
     *
     * @param vel_text   Codigo .vel completo.
     * @return           Documento HTML completo.
     */
    std::string html_from_vel_text(const std::string &vel_text);

} // namespace vex

#endif // VEX_HTML_DIAGRAMS_H
