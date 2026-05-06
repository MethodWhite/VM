/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file liveness.cpp
 * @brief Implementacion del analisis de vivacidad para la SSA IR.
 */

#include "ir/liveness.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ir {

LivenessResult compute_liveness(const IrFunction &fn) {
    LivenessResult result;
    const size_t nblocks = fn.blocks.size();

    result.block_start.resize(nblocks, 0);
    result.block_end.resize(nblocks, 0);
    result.num_instrs = 0;

    if (nblocks == 0) return result;

    // --- Paso 1: asignar posicion lineal a cada instruccion ---
    // Cada bloque ocupa [block_start[b], block_end[b]] inclusive.
    // Un bloque vacio igual ocupa una posicion (para que los predecesores
    // puedan extender intervalos hasta el "final" de ese bloque).
    uint32_t pos = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        result.block_start[b] = pos;
        uint32_t cnt = static_cast<uint32_t>(fn.blocks[b].instrs.size());
        // al menos una posicion por bloque
        uint32_t span = (cnt > 0) ? cnt : 1;
        pos += span;
        result.block_end[b] = pos - 1;
    }
    result.num_instrs = pos;

    const uint32_t UNDEF = result.num_instrs; // sentinel: valor no definido aun

    // --- Paso 2: inicializar un intervalo por valor ---
    // Usamos un mapa temporal; al final lo convertimos a vector.
    std::unordered_map<IrValueId, LiveInterval> imap;
    imap.reserve(fn.values.size());
    for (const auto &v : fn.values) {
        LiveInterval li;
        li.id  = v.id;
        li.def = UNDEF; // no definido aun
        li.end = 0;
        imap[v.id] = li;
    }

    // Los parametros de la funcion estan vivos desde el inicio (posicion 0).
    for (IrValueId pid : fn.params) {
        auto it = imap.find(pid);
        if (it != imap.end()) {
            it->second.def = 0;
            it->second.end = 0; // se actualizara con los usos reales
        }
    }

    // Marca un valor como "usado" en la posicion pos.
    //
    // IMPORTANTE: NO tocar def aqui.  En presencia de phi_args con back-edge
    // (loops), el "uso" como argumento phi se procesa cuando se recorre el
    // bloque sucesor (header), pero la definicion del valor esta en el
    // bloque predecesor (body), que aparece DESPUES en el orden lineal.
    // Si seteasemos def=use_pos cuando todavia es UNDEF, machacariamos el
    // def correcto que vendra cuando el liveness procese el body, dejando
    // intervalos colapsados [end,end] que confunden al linear scan y
    // provocan que reuse el mismo registro para valores realmente vivos
    // a la vez (el bug observado del while: sum_next y i_phi compartian r4).
    //
    auto mark_use = [&](IrValueId vid, uint32_t use_pos) {
        if (vid == IR_NO_VALUE) return;
        auto it = imap.find(vid);
        if (it == imap.end()) return;
        if (use_pos > it->second.end) it->second.end = use_pos;
    };

    // Marca la definicion de un valor en la posicion pos.
    auto mark_def = [&](IrValueId vid, uint32_t def_pos) {
        if (vid == IR_NO_VALUE) return;
        auto it = imap.find(vid);
        if (it == imap.end()) return;
        if (it->second.def == UNDEF) it->second.def = def_pos;
        // en SSA la definicion ocurre exactamente una vez; ignoramos redefiniciones
    };

    // --- Paso 3: recorrer todas las instrucciones ---
    for (size_t b = 0; b < nblocks; ++b) {
        const IrBlock &bb = fn.blocks[b];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            uint32_t instr_pos = result.block_start[b] + static_cast<uint32_t>(i);
            const IrInstr &ins = bb.instrs[i];

            // Definicion: el valor destino nace aqui
            mark_def(ins.dst, instr_pos);

            // Usos de operandos normales
            for (IrValueId op : ins.operands) {
                mark_use(op, instr_pos);
            }

            // Uso del puntero de funcion en CALLIND y CALLCLOSURE.  Sin
            // tratar este uso explicitamente, el liveness no extiende el
            // live range del SSA value que produjo fn_addr y el regalloc
            // puede reasignar su registro antes del call -> callvmr a
            // direccion basura.
            if (ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE) {
                mark_use(ins.func_ptr, instr_pos);
            }

            // Argumentos phi: el valor V que llega desde el bloque P
            // se considera "usado" al FINAL del bloque P (no en la instruccion phi).
            // Esto modela correctamente que V debe estar en un registro al salir de P.
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                uint32_t pred_end = (pa.block < static_cast<IrBlockId>(nblocks))
                                    ? result.block_end[pa.block]
                                    : instr_pos;
                mark_use(pa.value, pred_end);
            }
        }
    }

    // --- live-in / live-out por bloque (dataflow iterativo) ---
    //
    // Sin este pase, los valores definidos antes de un loop y usados dentro
    // del header se consideran "muertos" tras el ultimo uso lineal en el
    // body, por lo que el regalloc reusa su registro y rompe la siguiente
    // iteracion (bug observado con `i32* p = &x` dentro de un while).
    //
    // Algoritmo clasico de dataflow:
    //   use[B] = valores que el bloque B usa antes de definirlos
    //   def[B] = valores que B define
    //   live_out[B] = U_{S in succs(B)}  live_in[S]  U  {phi.value que llegan a S desde B}
    //   live_in[B]  = use[B] U (live_out[B] - def[B])
    // Iteramos en orden inverso hasta fixed point (rapido en CFGs reducibles).
    std::vector<std::unordered_set<IrValueId>> block_use(nblocks);
    std::vector<std::unordered_set<IrValueId>> block_def(nblocks);
    for (size_t b = 0; b < nblocks; ++b) {
        const IrBlock &bb = fn.blocks[b];
        for (const IrInstr &ins : bb.instrs) {
            // Cada operando es un uso si todavia no fue definido en B.
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                if (!block_def[b].count(op)) block_use[b].insert(op);
            }
            // Mismo razonamiento que arriba para CALLCLOSURE en el
            // dataflow de live-in/live-out por bloque: tratamos el
            // func_ptr como uso si no fue definido en el bloque.
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                if (!block_def[b].count(ins.func_ptr))
                    block_use[b].insert(ins.func_ptr);
            }
            // El destino se considera definido a partir de aqui.
            if (ins.dst != IR_NO_VALUE) {
                block_def[b].insert(ins.dst);
            }
            // phi_args se procesan como uses en el predecesor (mas abajo).
        }
    }

    std::vector<std::unordered_set<IrValueId>> live_in(nblocks);
    std::vector<std::unordered_set<IrValueId>> live_out(nblocks);
    bool changed = true;
    while (changed) {
        changed = false;
        // Recorrido en orden inverso ayuda a converger rapido en bucles.
        for (int b = static_cast<int>(nblocks) - 1; b >= 0; --b) {
            std::unordered_set<IrValueId> new_out;
            // live_in de cada sucesor.
            for (IrBlockId s : fn.blocks[b].succs) {
                if (s >= static_cast<IrBlockId>(nblocks)) continue;
                for (IrValueId v : live_in[s]) new_out.insert(v);
                // PHI args en el sucesor que vengan de este bloque b: el
                // valor entregado se debe poder leer al salir de b, asi
                // que esta vivo en live_out[b].
                for (const IrInstr &ins_s : fn.blocks[s].instrs) {
                    if (ins_s.op != IrOp::PHI) continue;
                    for (const auto &pa : ins_s.phi_args) {
                        if (pa.block == static_cast<IrBlockId>(b)
                         && pa.value != IR_NO_VALUE) {
                            new_out.insert(pa.value);
                        }
                    }
                }
            }
            // live_in[b] = use[b] U (new_out - def[b])
            std::unordered_set<IrValueId> new_in = block_use[b];
            for (IrValueId v : new_out) {
                if (!block_def[b].count(v)) new_in.insert(v);
            }
            if (new_out != live_out[b]) {
                live_out[b] = std::move(new_out);
                changed = true;
            }
            if (new_in != live_in[b]) {
                live_in[b] = std::move(new_in);
                changed = true;
            }
        }
    }

    // Extender intervalos: cada valor vivo a la salida de un bloque debe
    // mantener su registro al menos hasta el final de ese bloque (incluyendo
    // back-edges hacia loop headers).
    for (size_t b = 0; b < nblocks; ++b) {
        const uint32_t end_pos = result.block_end[b];
        for (IrValueId v : live_out[b]) {
            auto it = imap.find(v);
            if (it == imap.end()) continue;
            if (end_pos > it->second.end) it->second.end = end_pos;
        }
    }

    // Loop carry fix: si un value @c v se USA en una posicion ANTERIOR
    // a su definicion lineal (caso clasico de back-edge: %v def en
    // bloque B6, usado como PHI arg desde bloque B3 que viene antes
    // linealmente), extender el end al final de la funcion para que el
    // linear scan no reuse el reg de @c v en cualquier bloque entre
    // B3 y B6 (que en orden de ejecucion del CFG ESTA en el body del
    // loop, accesible via back-edge).
    //
    // Sin esto, el regalloc reusa el reg de @c v dentro de step/body y
    // los PHI moves al final del back-edge leen basura -> resultados
    // incorrectos en for/while con vars carry-loop.

    // --- Paso 4: asegurar coherencia en los parametros ---
    // Si un parametro nunca se uso, su end quedaria a 0 < def=0.
    for (IrValueId pid : fn.params) {
        auto it = imap.find(pid);
        if (it != imap.end() && it->second.def == 0 && it->second.end < it->second.def) {
            it->second.end = it->second.def;
        }
    }

    // --- Paso 5: recopilar solo los valores con definicion conocida ---
    result.intervals.reserve(imap.size());
    for (auto &[id, li] : imap) {
        if (li.def < UNDEF) {
            // Garantizar end >= def (un valor usado exactamente en su def tiene end==def)
            if (li.end < li.def) li.end = li.def;
            result.intervals.push_back(li);
        }
    }

    // Ordenar por posicion de definicion (requerido por linear scan)
    std::sort(result.intervals.begin(), result.intervals.end());

    return result;
}

} // namespace ir
