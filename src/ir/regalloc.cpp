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
#include <unordered_map>
#include <unordered_set>

namespace ir {

// =========================================================================
//  Register hints para cadenas de binops
// =========================================================================
// Para una secuencia tipica como:
//    %a = mul %x, %y
//    %b = add %z, %a
//    %c = xor %b, %k
// El emit_binop produce `mov rd, rs1; op rd, rs2` que costa 2 instr VM por
// op.  Si el regalloc coloca `%b` en el mismo reg que `%a` (ya que `%a` muere
// al consumirse en el add), `mov rd, rs1` desaparece (`emit_mov_if_needed`
// detecta rd==rs1).  Esto AHORRA 1 instr VM por op encadenada.
//
// Estrategia: pre-pasada que para cada binop/unop computa un "hint" del
// registro que el dst PREFIERE: el reg del primer operando IFF ese operando
// muere en esta instruccion (su intervalo termina aqui Y no es param/usado
// en otros bloques via phi).
//
// El linear scan, al expirar el intervalo del operando, comprueba si hay un
// valor que lo este "esperando" via hint.  Si si, ese reg se prioriza para
// el siguiente alloc (en lugar de un reg arbitrario).
struct RegHint {
    IrValueId pref_via_value = IR_NO_VALUE; // %dst quiere el reg de pref_via_value
};

// Devuelve true si el `op` es un binop/unop two-address donde el primer
// operando se "consume" en rd (es decir, el emit hace `mov rd, src1; op rd, src2`
// o `mov rd, src; op rd`).  Estos son los candidatos a hinting porque el
// MOV se elimina si rd == src1's reg.
static bool is_two_address_op(IrOp op) {
    switch (op) {
        case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
        case IrOp::DIV: case IrOp::MOD:
        case IrOp::AND: case IrOp::OR:  case IrOp::XOR:
        case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
        case IrOp::NEG: case IrOp::NOT:
        case IrOp::MOV:
        case IrOp::SEXT: case IrOp::ZEXT: case IrOp::TRUNC:
        case IrOp::CAST: case IrOp::BITCAST:
            return true;
        default:
            return false;
    }
}

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

    // ---- Computar register hints para cadenas de binops ----
    // hint_for[dst_vid] = src1_vid si el binop al definir dst tiene src1
    // que muere en esa misma instruccion (single use, no usado en otro
    // bloque, no es param).  El alloc preferira el reg de src1 cuando
    // este disponible (que lo estara, recien expirado).
    std::unordered_map<IrValueId, IrValueId> hint_for;
    {
        // Mapa rapido vid -> interval.end
        std::unordered_map<IrValueId, uint32_t> end_of;
        for (const auto &li : liveness.intervals) end_of[li.id] = li.end;

        // Set de params (no hintear sobre params; sus regs son fijos)
        std::unordered_set<IrValueId> param_set(fn.params.begin(), fn.params.end());

        // Walk linealizado paralelo a liveness.  Mismo orden que el liveness.
        uint32_t pos = 0;
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const auto &bb = fn.blocks[bi];
            const size_t span = bb.instrs.empty() ? 1 : bb.instrs.size();
            for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
                const auto &ins = bb.instrs[ii];
                const uint32_t cur_pos = pos + static_cast<uint32_t>(ii);
                if (ins.dst == IR_NO_VALUE) continue;
                if (!is_two_address_op(ins.op)) continue;
                if (ins.operands.empty()) continue;
                IrValueId src1 = ins.operands[0];
                if (src1 == IR_NO_VALUE) continue;
                if (param_set.count(src1)) continue;
                auto it = end_of.find(src1);
                if (it == end_of.end()) continue;
                // src1 muere exactamente aqui (su ultimo uso es cur_pos)
                if (it->second != cur_pos) continue;
                hint_for[ins.dst] = src1;
            }
            pos += static_cast<uint32_t>(span);
        }
    }
    // ---- Fin hints ----

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
                // CRITICAL Bug C fix: borrar reg_map[spilled.id] porque
                // el reg pasa a `li.id`.  Sin esto, el desalojado quedaria
                // en ambos reg_map (apuntando a reg ya reusado) y
                // spill_map.  load_src/dst_of priorizan reg_map (lookup
                // primero) y retornarian un reg con valor de OTRO IrValueId
                // tras la reasignacion -> linked list / PHI nodes con
                // punteros stale tras un CALL.
                result.reg_map.erase(spilled.id);
                // reusar el registro del desalojado para li
                result.reg_map[li.id] = spilled.reg;
                active.insert({li.end, li.id, spilled.reg});
            } else {
                // El intervalo actual tiene el mayor end; lo derramamos directamente
                result.spill_map[li.id] = result.spill_count++;
            }
        } else {
            // Asignar registro: si hay hint, intentar reusar el reg del
            // operando que muere en esta instruccion.  Dos casos:
            //   (a) hinted vid ya esta en free_pool (expirado en pasada
            //       previa por otra razon).
            //   (b) hinted vid sigue en `active` pero su .end == li.def
            //       -- es decir, esta instruccion es su ultimo uso.  Es
            //       seguro robarle el reg AHORA porque el flujo es:
            //       op rd, rs (lee rs, escribe rd).  Si rd == rs's reg,
            //       primero se lee rs, luego se escribe el resultado en
            //       el mismo reg.  El emitter ve rd == rs1 y elide el
            //       MOV intermedio.  Coalesce gratis.
            int reg = -1;
            auto hit = hint_for.find(li.id);
            if (hit != hint_for.end()) {
                auto rm = result.reg_map.find(hit->second);
                if (rm != result.reg_map.end()) {
                    int pref = rm->second;
                    auto pit = std::find(free_pool.begin(), free_pool.end(), pref);
                    if (pit != free_pool.end()) {
                        reg = pref;
                        free_pool.erase(pit);
                    } else {
                        // Buscar en active si el hinted vid sigue ahi con
                        // end == li.def (= termina justo en este uso).
                        auto ait = active.end();
                        for (auto a = active.begin(); a != active.end(); ++a) {
                            if (a->id == hit->second && a->end == li.def) {
                                ait = a;
                                break;
                            }
                        }
                        if (ait != active.end()) {
                            // Steal: extraer y reusar.  No devolver al free_pool
                            // (lo entregamos directamente a li.id).
                            reg = ait->reg;
                            active.erase(ait);
                        }
                    }
                }
            }
            if (reg < 0) {
                reg = free_pool.back();
                free_pool.pop_back();
            }
            result.reg_map[li.id] = reg;
            active.insert({li.end, li.id, reg});
        }
    }

    return result;
}

} // namespace ir
