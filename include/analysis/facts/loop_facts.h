/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/loop_facts.h
 * @brief LoopFacts: hechos de BUCLES por bloque, UNIFICADOS en un productor.
 *
 * Antes de esto, la deteccion de bucles estaba DUPLICADA ad-hoc en varios pases
 * (SROA e LICM en @c ir_optimizer.cpp, cada uno con su copia).  El modelo de
 * banco ancho lo destapo como deuda tecnica: el mismo hecho recomputado en
 * varios sitios sin una fuente unica.  @c LoopFacts centraliza esa deteccion:
 * un solo productor que TODOS los consumidores (LICM, SROA, vectorizador,
 * scheduler, profile, unroller, y el LoopAdapter del banco ancho) pueden
 * consultar -- "todos preguntan al mismo sitio".
 *
 * Se llama @c LoopFacts (no @c LoopInfo) porque es un HECHO derivado del CFG:
 * hoy rellena la profundidad + header + in-loop + id de bucle, pero la struct
 * tiene SITIO PARA CRECER (trip-count, pre-header, tipo de bucle) sin cambiar
 * su rol.
 *
 * ALGORITMO (el mismo canonico que ya usaban SROA/LICM, ahora en un sitio): CFG
 * desde los terminadores -> dominadores (Cooper-Harvey-Kennedy) -> back-edges
 * (arista b->h con h dominando b) -> cuerpo del bucle (BFS inverso por preds) ->
 * profundidad = numero de bucles que contienen el bloque.
 *
 * NOTA (dispersion aun mas profunda, documentada): los dominadores tambien viven
 * como @c DomInfo LOCAL en @c ir_optimizer.cpp.  @c LoopFacts los computa
 * INTERNAMENTE por ahora (autocontenido); un futuro @c DomFacts los centralizara
 * y entonces @c LoopFacts, SROA e LICM los compartiran (patron strangler-fig:
 * primero el productor unificado, luego migran los consumidores).
 */

#ifndef VESTA_ANALYSIS_FACTS_LOOP_FACTS_H
#define VESTA_ANALYSIS_FACTS_LOOP_FACTS_H

#include "analysis/fact_validation.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @struct LoopFacts
 * @brief Hechos de bucles por bloque de una funcion (indexado por IrBlockId).
 */
struct LoopFacts {
    /// Sentinela "el bloque no pertenece a ningun bucle".
    static constexpr uint32_t NO_LOOP = 0xFFFFFFFFu;

    // --- Hechos POR BLOQUE (indexados por IrBlockId) ---
    std::vector<uint32_t> loop_depth;     ///< profundidad de anidamiento (0 = fuera).
    std::vector<uint8_t>  is_loop_header; ///< 1 si el bloque es cabecera de un bucle.
    std::vector<uint8_t>  in_loop;        ///< 1 si el bloque esta dentro de algun bucle.
    std::vector<uint32_t> loop_id;        ///< id del bucle MAS INTERNO que lo contiene (o NO_LOOP).

    // --- Hechos POR BUCLE (indexados por id de bucle 0..loop_count-1) ---
    std::vector<uint32_t> loop_header;    ///< bloque cabecera de cada bucle.
    std::vector<uint32_t> parent_loop;    ///< bucle que CONTIENE a cada bucle (o NO_LOOP).
                                          ///< Para unroller/vectorizer/scheduler:
                                          ///< "¿quien contiene este bucle?".
    uint32_t              loop_count = 0; ///< numero de bucles naturales detectados.

    // --- Consultas seguras por bloque (fuera de rango -> valor neutro) ---
    uint32_t depth_of(ir::IrBlockId b) const noexcept {
        return b < loop_depth.size() ? loop_depth[b] : 0;
    }
    bool header_of(ir::IrBlockId b) const noexcept {
        return b < is_loop_header.size() && is_loop_header[b] != 0;
    }
    bool inside(ir::IrBlockId b) const noexcept {
        return b < in_loop.size() && in_loop[b] != 0;
    }
    uint32_t innermost(ir::IrBlockId b) const noexcept {
        return b < loop_id.size() ? loop_id[b] : NO_LOOP;
    }

    // --- Consultas seguras por bucle ---
    uint32_t parent_of(uint32_t loop) const noexcept {
        return loop < parent_loop.size() ? parent_loop[loop] : NO_LOOP;
    }
    uint32_t header_block_of(uint32_t loop) const noexcept {
        return loop < loop_header.size() ? loop_header[loop]
                                         : static_cast<uint32_t>(ir::IR_NO_BLOCK);
    }
};

/**
 * @brief Computa los @c LoopFacts de una funcion desde su CFG.
 * @param fn  funcion SSA (bloques con terminadores BR/BR_COND/SWITCH/RET).
 * @return    hechos por bloque; vectores dimensionados a @c fn.blocks.size().
 */
LoopFacts compute_loop_facts(const ir::IrFunction &fn);

/**
 * @brief AUTOCERTIFICACION: comprueba los invariantes internos de @p f.
 * @return lista de violaciones (vacia = LoopFacts consistente).
 *
 * Invariantes: un header esta in_loop; @c loop_depth>0 <=> @c in_loop; los
 * @c loop_id/@c parent_loop/@c header_block estan en rango; ningun bucle es su
 * propio padre.
 */
std::vector<FactIssue> validate(const LoopFacts &f);

} // namespace analysis

#endif // VESTA_ANALYSIS_FACTS_LOOP_FACTS_H
