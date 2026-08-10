/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/loop_facts.cpp
 * @brief Implementacion de @c compute_loop_facts (ver loop_facts.h).
 *
 * CFG desde terminadores -> dominadores (Cooper-Harvey-Kennedy) -> back-edges ->
 * cuerpos de bucle (BFS inverso) -> profundidad por bloque.  Mismo algoritmo
 * canonico que SROA/LICM usaban por separado, ahora unificado.
 */

#include "analysis/facts/loop_facts.h"

#include <cstdint>
#include <vector>

namespace analysis {

using ir::IrBlockId;
using ir::IrFunction;

namespace {

/** @brief Sucesores de cada bloque, tomados de los terminadores. */
std::vector<std::vector<IrBlockId>> build_succs(const IrFunction &fn) {
    const size_t N = fn.blocks.size();
    std::vector<std::vector<IrBlockId>> succs(N);
    auto add = [&](std::vector<IrBlockId> &v, IrBlockId t) {
        if (t == ir::IR_NO_BLOCK || static_cast<size_t>(t) >= N) return;
        for (IrBlockId x : v)
            if (x == t) return; // dedup
        v.push_back(t);
    };
    for (size_t b = 0; b < N; ++b) {
        for (const ir::IrInstr &ins : fn.blocks[b].instrs) {
            add(succs[b], ins.target_block);
            add(succs[b], ins.false_block);
        }
    }
    return succs;
}

/** @brief Predecesores = inversa de los sucesores. */
std::vector<std::vector<IrBlockId>>
build_preds(const std::vector<std::vector<IrBlockId>> &succs) {
    std::vector<std::vector<IrBlockId>> preds(succs.size());
    for (size_t b = 0; b < succs.size(); ++b)
        for (IrBlockId s : succs[b])
            preds[s].push_back(static_cast<IrBlockId>(b));
    return preds;
}

/**
 * @brief Numeracion postorden por DFS desde @p entry (iterativo).
 * @param po      salida: po[b] = numero postorden, o UINT32_MAX si inalcanzable.
 * @param rpo     salida: bloques en reverse-postorden (solo alcanzables).
 */
void compute_rpo(const std::vector<std::vector<IrBlockId>> &succs,
                 IrBlockId entry, std::vector<uint32_t> &po,
                 std::vector<IrBlockId> &rpo) {
    const size_t N = succs.size();
    po.assign(N, UINT32_MAX);
    std::vector<uint8_t> visited(N, 0);
    std::vector<IrBlockId> order; // postorden
    // DFS iterativo con pila de (nodo, indice de sucesor).
    std::vector<std::pair<IrBlockId, size_t>> stk;
    if (static_cast<size_t>(entry) >= N) return;
    visited[entry] = 1;
    stk.push_back({entry, 0});
    while (!stk.empty()) {
        auto &top = stk.back();
        if (top.second < succs[top.first].size()) {
            IrBlockId s = succs[top.first][top.second++];
            if (!visited[s]) { visited[s] = 1; stk.push_back({s, 0}); }
        } else {
            order.push_back(top.first);
            stk.pop_back();
        }
    }
    uint32_t n = 0;
    for (IrBlockId b : order) po[b] = n++;
    // RPO = orden inverso del postorden.
    rpo.assign(order.rbegin(), order.rend());
}

/** @brief idom via CHK.  idom[b] = IR_NO_BLOCK si inalcanzable. */
std::vector<IrBlockId>
compute_idom(const std::vector<std::vector<IrBlockId>> &preds,
             const std::vector<uint32_t> &po,
             const std::vector<IrBlockId> &rpo, IrBlockId entry) {
    const size_t N = preds.size();
    std::vector<IrBlockId> idom(N, ir::IR_NO_BLOCK);
    if (rpo.empty()) return idom;
    idom[entry] = entry;

    auto intersect = [&](IrBlockId a, IrBlockId b) -> IrBlockId {
        while (a != b) {
            // Numeros postorden mas ALTOS = mas cerca de la entrada en RPO.
            while (po[a] < po[b]) a = idom[a];
            while (po[b] < po[a]) b = idom[b];
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = ir::IR_NO_BLOCK;
            for (IrBlockId p : preds[b]) {
                if (idom[p] == ir::IR_NO_BLOCK) continue; // pred aun sin procesar
                new_idom = (new_idom == ir::IR_NO_BLOCK) ? p : intersect(p, new_idom);
            }
            if (new_idom != ir::IR_NO_BLOCK && idom[b] != new_idom) {
                idom[b] = new_idom;
                changed = true;
            }
        }
    }
    return idom;
}

/** @brief True si @p a domina a @p b (recorre la cadena idom de @p b). */
bool dominates(const std::vector<IrBlockId> &idom, IrBlockId a, IrBlockId b) {
    if (idom[b] == ir::IR_NO_BLOCK) return false; // b inalcanzable
    IrBlockId cur = b;
    while (true) {
        if (cur == a) return true;
        if (idom[cur] == cur) return false; // llego a la entrada sin encontrar a
        cur = idom[cur];
    }
}

} // namespace

LoopFacts compute_loop_facts(const IrFunction &fn) {
    const size_t N = fn.blocks.size();
    LoopFacts f;
    f.loop_depth.assign(N, 0);
    f.is_loop_header.assign(N, 0);
    f.in_loop.assign(N, 0);
    f.loop_id.assign(N, LoopFacts::NO_LOOP);
    if (N == 0) return f;

    const IrBlockId entry = 0;
    auto succs = build_succs(fn);
    auto preds = build_preds(succs);
    std::vector<uint32_t> po;
    std::vector<IrBlockId> rpo;
    compute_rpo(succs, entry, po, rpo);
    auto idom = compute_idom(preds, po, rpo, entry);

    // Back-edges (b -> h con h dominando b), agrupados por cabecera = 1 bucle.
    struct Loop { IrBlockId header; std::vector<uint8_t> body; size_t size = 0; };
    std::vector<Loop> loops;
    std::vector<int32_t> loop_of_header(N, -1); // header -> indice en loops

    for (size_t b = 0; b < N; ++b) {
        for (IrBlockId h : succs[b]) {
            if (!dominates(idom, h, static_cast<IrBlockId>(b))) continue;
            // Back-edge b->h.  Obtener/crear el bucle de cabecera h.
            int32_t li = loop_of_header[h];
            if (li < 0) {
                loops.push_back(Loop{h, std::vector<uint8_t>(N, 0), 0});
                li = static_cast<int32_t>(loops.size()) - 1;
                loop_of_header[h] = li;
                loops[li].body[h] = 1;
            }
            Loop &lp = loops[li];
            // Cuerpo: BFS inverso desde b por preds, sin pasar de h.
            std::vector<IrBlockId> stk;
            if (!lp.body[b]) { lp.body[b] = 1; stk.push_back(static_cast<IrBlockId>(b)); }
            while (!stk.empty()) {
                IrBlockId x = stk.back(); stk.pop_back();
                if (x == h) continue;
                for (IrBlockId p : preds[x])
                    if (!lp.body[p]) { lp.body[p] = 1; if (p != h) stk.push_back(p); }
            }
        }
    }

    // Tamanos + hechos por bloque.
    for (Loop &lp : loops) {
        lp.size = 0;
        for (size_t b = 0; b < N; ++b) lp.size += lp.body[b];
    }
    f.loop_count = static_cast<uint32_t>(loops.size());
    for (size_t li = 0; li < loops.size(); ++li) {
        const Loop &lp = loops[li];
        f.is_loop_header[lp.header] = 1;
        for (size_t b = 0; b < N; ++b) {
            if (!lp.body[b]) continue;
            f.in_loop[b] = 1;
            f.loop_depth[b] += 1; // un bucle mas que contiene el bloque
            // loop_id = bucle MAS INTERNO (menor cuerpo) que contiene el bloque.
            uint32_t cur = f.loop_id[b];
            if (cur == LoopFacts::NO_LOOP || lp.size < loops[cur].size)
                f.loop_id[b] = static_cast<uint32_t>(li);
        }
    }

    // Hechos POR BUCLE: cabecera + bucle padre (el bucle mas pequeno que
    // CONTIENE PROPIAMENTE a este = su cabecera cae en el cuerpo de otro mayor).
    f.loop_header.resize(loops.size());
    f.parent_loop.assign(loops.size(), LoopFacts::NO_LOOP);
    for (size_t li = 0; li < loops.size(); ++li) {
        f.loop_header[li] = loops[li].header;
        const IrBlockId h = loops[li].header;
        uint32_t best = LoopFacts::NO_LOOP;
        size_t best_size = SIZE_MAX;
        for (size_t lj = 0; lj < loops.size(); ++lj) {
            if (lj == li) continue;
            if (!loops[lj].body[h]) continue;              // lj contiene la cabecera de li
            if (loops[lj].size <= loops[li].size) continue; // contencion PROPIA (mayor)
            if (loops[lj].size < best_size) { best_size = loops[lj].size; best = static_cast<uint32_t>(lj); }
        }
        f.parent_loop[li] = best;
    }
    return f;
}

std::vector<FactIssue> validate(const LoopFacts &f) {
    std::vector<FactIssue> issues;
    const size_t N = f.loop_depth.size();
    for (size_t b = 0; b < N; ++b) {
        const bool in = f.in_loop[b] != 0;
        const bool depth_pos = f.loop_depth[b] > 0;
        // depth>0 <=> in_loop.
        if (in != depth_pos)
            issues.push_back({FactCheck::LOOP_DEPTH_INLOOP_MISMATCH, b, 0});
        // Un header debe estar in_loop.
        if (f.is_loop_header[b] && !in)
            issues.push_back({FactCheck::LOOP_HEADER_NOT_IN_LOOP, b, 0});
        // loop_id en rango.
        if (f.loop_id[b] != LoopFacts::NO_LOOP && f.loop_id[b] >= f.loop_count)
            issues.push_back({FactCheck::LOOP_ID_OUT_OF_RANGE, b, f.loop_id[b]});
    }
    for (uint32_t L = 0; L < f.loop_count; ++L) {
        const uint32_t p = f.parent_of(L);
        if (p != LoopFacts::NO_LOOP && p >= f.loop_count)
            issues.push_back({FactCheck::LOOP_PARENT_OUT_OF_RANGE, L, p});
        if (p == L)
            issues.push_back({FactCheck::LOOP_PARENT_SELF, L, 0});
        if (L < f.loop_header.size() && f.loop_header[L] >= N)
            issues.push_back({FactCheck::LOOP_HEADER_BLOCK_OOR, L, f.loop_header[L]});
    }
    return issues;
}

} // namespace analysis
