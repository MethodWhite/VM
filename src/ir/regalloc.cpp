/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file regalloc.cpp
 * @brief Asignador de registros VM por barrido lineal (Poletto & Sarkar, 1999).
 */

#include "ir/regalloc.h"
#include <algorithm>
#include <set>
#include <vector>

namespace ir {

// --- tabla de nombres de registros ---
static const char *REG_NAMES[16] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};

const char *reg_name(int reg) {
    if (reg >= 0 && reg < 16) return REG_NAMES[reg];
    return "r?";
}

// =========================================================================
//  Algoritmo de barrido lineal
// =========================================================================

AllocResult allocate_regs(const IrFunction &fn, const LivenessResult &liveness) {
    AllocResult result;
    result.spill_count = 0;
    result.ok          = true;

    if (liveness.intervals.empty()) return result;

    // Conjunto de registros libres (r0-r13).
    // Usamos r1-r13 como pool principal; r0 se reserva para retorno pero
    // puede asignarse a valores no parametro.
    std::vector<int> free_pool;
    free_pool.reserve(ALLOC_REGS);
    for (int r = ALLOC_REGS - 1; r >= 0; --r) {
        // insertar en orden descendente para que el pop_back tome r1 primero
        free_pool.push_back(r);
    }
    // r0 al final del pool (lo asignamos ultimo para reservarlo para retorno)
    // Truco: mover r0 al final del vector para que se use con menor prioridad
    std::rotate(free_pool.begin(),
                std::find(free_pool.begin(), free_pool.end(), 0),
                free_pool.end());
    // Ahora free_pool = [1, 2, 3, ..., 13, 0]

    // Pre-asignar parametros a r1, r2, ..., r12 (convencion de llamada)
    {
        size_t pi = 0;
        for (IrValueId pid : fn.params) {
            if (pi >= 12) break; // maximo 12 parametros en registros r1-r12
            int preg = static_cast<int>(pi + 1); // r1, r2, ...
            result.reg_map[pid] = preg;
            // quitar preg del pool libre
            auto it = std::find(free_pool.begin(), free_pool.end(), preg);
            if (it != free_pool.end()) free_pool.erase(it);
            ++pi;
        }
        // parametros extra (>12) van directamente a spill
        for (; pi < fn.params.size(); ++pi) {
            IrValueId pid = fn.params[pi];
            result.spill_map[pid] = result.spill_count++;
        }
    }

    // Conjunto activo: intervalos en vuelo, ordenados por end para facilitar
    // la eleccion del candidato a desalojar.
    struct ActiveEntry {
        uint32_t  end;
        IrValueId id;
        int       reg;
        bool operator<(const ActiveEntry &o) const {
            return end < o.end || (end == o.end && id < o.id);
        }
    };
    std::set<ActiveEntry> active;

    // Agregar al conjunto activo los parametros ya pre-asignados
    for (const auto &li : liveness.intervals) {
        auto rm = result.reg_map.find(li.id);
        if (rm != result.reg_map.end()) {
            active.insert({li.end, li.id, rm->second});
        }
    }

    // Barrido lineal sobre intervalos ordenados por def
    for (const auto &li : liveness.intervals) {
        // Saltar valores ya asignados (parametros pre-asignados o extra-spill)
        if (result.reg_map.count(li.id) || result.spill_map.count(li.id)) continue;

        // Expirar intervalos cuyo end < li.def (ya no estan vivos)
        std::vector<ActiveEntry> expired;
        for (const auto &a : active) {
            if (a.end < li.def) expired.push_back(a);
            else break; // el set esta ordenado por end, podemos parar
        }
        for (const auto &a : expired) {
            active.erase(a);
            free_pool.push_back(a.reg); // devolver registro al pool
        }

        if (free_pool.empty()) {
            // No hay registro libre: desalojar el intervalo con mayor end
            // (estrategia de "spill the farthest")
            auto it_max = active.end();
            --it_max; // el ultimo en el set ordenado por end

            if (it_max->end > li.end) {
                // Desalojar el activo con mayor end; su registro pasa a li
                ActiveEntry spilled = *it_max;
                active.erase(it_max);
                result.spill_map[spilled.id] = result.spill_count++;
                // reusar el registro del desalojado para li
                result.reg_map[li.id] = spilled.reg;
                active.insert({li.end, li.id, spilled.reg});
            } else {
                // El intervalo actual tiene el mayor end; lo derramamos directamente
                result.spill_map[li.id] = result.spill_count++;
            }
        } else {
            // Asignar el ultimo registro disponible del pool
            int reg = free_pool.back();
            free_pool.pop_back();
            result.reg_map[li.id] = reg;
            active.insert({li.end, li.id, reg});
        }
    }

    return result;
}

} // namespace ir
