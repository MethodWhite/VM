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
 * @file asm_diagram.h
 * @brief Diagramas del CFG de un bloque de inline asm anotados con coste
 *        (latencia, cuello de botella por puerto), flags y diagnosticos.
 *
 * Compone el CFG (ver vx/asm_cfg.h), el modelo de coste superescalar por
 * microarquitectura (ver vx/instr_db.h: @c analyze_asm_cost) y los diagnosticos
 * (ver vx/asm_diag.h) en un subgrafo listo para EMBEBER dentro de los diagramas
 * del compilador (mermaid / graphviz).  Cada bloque basico muestra sus
 * instrucciones + su latencia (camino critico, cota superior) + su throughput
 * (cota inferior superescalar) + el puerto de ejecucion mas cargado (cuello de
 * botella).  Un encabezado resume el bloque entero y lista los diagnosticos.
 */

#ifndef VX_ASM_DIAGRAM_H
#define VX_ASM_DIAGRAM_H

#include <cstdint>
#include <string>

#include "vex/asm/instr_db.h"

namespace vex {

/// Opciones de generacion de un diagrama de asm.
struct AsmDiagramOptions {
    instr_db::Isa isa = instr_db::Isa::X86;
    uint32_t ua_id = 0;             ///< microarquitectura (indice) para el coste.
    std::string microarch;          ///< nombre de la microarq (para el titulo).
    std::string id_prefix = "asm";  ///< prefijo unico de los ids de nodo.
    std::string title;              ///< titulo opcional del subgrafo.
};

/**
 * @brief Genera el CFG del bloque @p body como SUBGRAFO mermaid embebible.
 *
 * Devuelve las lineas @c subgraph...end (nodos de bloque basico + aristas +
 * un nodo-resumen con el coste total y los diagnosticos).  Listo para insertar
 * dentro de un diagrama mermaid mayor.
 */
std::string asm_cfg_mermaid(const std::string &body,
                            const AsmDiagramOptions &opt);

/**
 * @brief Genera el CFG del bloque @p body como CLUSTER graphviz embebible.
 *
 * Devuelve las lineas @c subgraph cluster_...{...} (mismo contenido que la
 * variante mermaid) para insertar dentro de un @c digraph mayor.
 */
std::string asm_cfg_graphviz(const std::string &body,
                             const AsmDiagramOptions &opt);

} // namespace vex

#endif // VX_ASM_DIAGRAM_H
