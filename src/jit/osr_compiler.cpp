/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_compiler.cpp
 * @brief Implementacion del bridge OSR: extraccion de bucles y
 *        compilacion con entry point OSR.
 *
 * = Diseno =
 *
 * El OsrCompiler toma una solicitud de compilacion OSR y produce
 * codigo nativo con un punto de entrada secundario (OSR entry) que
 * permite reanudar la ejecucion a mitad de la funcion, en el loop
 * header, con un estado parcial.
 *
 * Para ello:
 *
 *   1. Extrae el cuerpo del bucle como una funcion IR independiente
 *      (extract_loop_body).
 *
 *   2. Decide el tier de compilacion (C1 o C2) segun la hotness.
 *
 *   3. Compila con JitCompiler, pero indicando al selector que debe
 *      emitir un OSR entry point en el loop header (via OsrEmit).
 *
 *   4. El resultado es un OsrEntry con la direccion del entry point.
 *
 *   5. Registra la entrada en la tabla OSR global para que el monitor
 *      pueda encontrarla.
 *
 * = Extraccion del bucle =
 *
 * Dada una funcion IR con bloques:
 *
 *   entry:    ... br header
 *   header:   phi ..., ...; cmp; br_cond body, exit
 *   body:     ...; br header      (back-edge)
 *   exit:     ...; ret
 *
 * Se extrae {header, body} como una nueva funcion donde:
 *   - Los operandos de PHI que vienen de fuera (entry) se convierten
 *     en parametros de la nueva funcion.
 *   - Se anyade un prologo que lee los parametros del osr_buffer.
 *
 * = Stackmaps =
 *
 * En el punto OSR entry, se emite un stackmap que describe las
 * ubicaciones de los GC roots vivos (GcHandles) para que el GC
 * pueda escanear precisamente incluso durante la ejecucion OSR.
 */

#include "jit/osr_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/jit_registry.h"
#include "jit/osr_data.h"
#include "jit/osr_monitor.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"
#include "jit/vreg_pipeline.h"
#include "vesta_rt/abi.h"
#include "vesta_rt/public.h"

namespace jit {

    // =====================================================================
    //  Seleccion de tier
    // =====================================================================

    OsrCompileTier OsrCompiler::select_tier(uint32_t back_edge_count) {
        /* Umbrales heuristicos:
         *   < 10,000:    no compilar (seguir interpretando)
         *   10,000-99,999:  C1 (compilacion rapida)
         *   >= 100,000:     C2 (compilacion optimizada)
         */
        if (back_edge_count < OSR_DEFAULT_THRESHOLD) {
            return OsrCompileTier::C1_INTERPRETED;
        }
        if (back_edge_count < OSR_DEFAULT_THRESHOLD * 10) {
            return OsrCompileTier::C1_LOOP;
        }
        return OsrCompileTier::C2_OPTIMIZED;
    }

    // =====================================================================
    //  Extraccion del cuerpo del bucle
    // =====================================================================

    bool OsrCompiler::extract_loop_body(const ir::IrFunction &source,
                                         uint32_t header_block,
                                         ir::IrFunction &out_body) {
        if (header_block >= source.blocks.size()) {
            return false;
        }

        /* Identificar los bloques que pertenecen al bucle:
         * - header_block es el loop header.
         * - Cualquier bloque alcanzable desde header_block sin pasar
         *   por un bloque que domine a header_block (aproximacion:
         *   BFS desde header_block hasta encontrar back-edge a header). */
        std::unordered_set<uint32_t> loop_blocks;
        std::vector<uint32_t> worklist;
        worklist.push_back(header_block);
        loop_blocks.insert(header_block);

        while (!worklist.empty()) {
            const uint32_t bid = worklist.back();
            worklist.pop_back();
            const auto &block = source.blocks[bid];
            for (const auto &succ : block.succs) {
                if (succ == header_block) {
                    /* Back-edge al header: el bloque pertenece al bucle. */
                    loop_blocks.insert(bid);
                    continue;
                }
                if (succ >= source.blocks.size()) continue;
                if (loop_blocks.count(succ)) continue;
                /* No explorar bloques que esten fuera del bucle
                 * (succ > header_block y no alcanzable desde header
                 *  sin pasar por el header de nuevo).  En la practica,
                 * los bloques del bucle son {header, body, ...} que
                 * estan entre header_block y el/los bloques con back-edge. */
                if (succ > header_block) {
                    loop_blocks.insert(succ);
                    worklist.push_back(succ);
                }
            }
        }

        if (loop_blocks.size() < 2) {
            /* Un bucle debe tener al menos header + 1 bloque. */
            return false;
        }

        /* Construir la nueva funcion: copiar solo los bloques del bucle. */
        out_body.name = source.name + "_osr_body";
        out_body.ret_type = source.ret_type;

        /* Mapa: block_id original -> nuevo block_id. */
        std::unordered_map<uint32_t, uint32_t> block_map;
        for (uint32_t bid : loop_blocks) {
            const uint32_t new_id = out_body.new_block(
                source.blocks[bid].name + "_osr");
            block_map[bid] = new_id;
        }

        /* El bloque header en la nueva funcion recibe los valores
         * entrantes (PHI desde fuera) como parametros.  Identificamos
         * los valores que entran desde bloques fuera del bucle. */
        std::unordered_set<uint32_t> external_params;
        const auto &header = source.blocks[header_block];
        for (const auto &instr : header.instrs) {
            if (instr.op == ir::IrOp::PHI) {
                for (const auto &phi_arg : instr.phi_args) {
                    if (!loop_blocks.count(phi_arg.block)) {
                        /* El valor viene de fuera del bucle -> es parametro. */
                        external_params.insert(instr.dst);
                    }
                }
            }
        }

        /* Clonar las instrucciones de cada bloque del bucle. */
        for (uint32_t bid : loop_blocks) {
            const auto &src_block = source.blocks[bid];
            const uint32_t new_bid = block_map[bid];
            auto &dst_block = out_body.blocks[new_bid];

            for (const auto &instr : src_block.instrs) {
                ir::IrInstr new_instr = instr;

                /* Remapear operandos de PHI: eliminar entradas de
                 * bloques fuera del bucle. */
                if (instr.op == ir::IrOp::PHI && bid == header_block) {
                    std::vector<ir::IrPhiArg> filtered_args;
                    for (const auto &arg : instr.phi_args) {
                        if (loop_blocks.count(arg.block)) {
                            /* Remapear block id. */
                            auto it = block_map.find(arg.block);
                            if (it != block_map.end()) {
                                filtered_args.push_back(
                                    {arg.value, it->second});
                            }
                        }
                    }
                    if (filtered_args.empty()) {
                        /* Si todos los PHI args eran externos, el valor
                         * viene de parametro.  No emitimos el PHI. */
                        continue;
                    }
                    new_instr.phi_args = std::move(filtered_args);
                }

                /* Remapear target_block y false_block. */
                {
                    auto it = block_map.find(instr.target_block);
                    if (it != block_map.end()) {
                        new_instr.target_block = it->second;
                    }
                }
                {
                    auto it = block_map.find(instr.false_block);
                    if (it != block_map.end()) {
                        new_instr.false_block = it->second;
                    }
                }

                /* Remapear operandos de bloque en phi_args ya hecho. */

                out_body.append(new_bid, std::move(new_instr));
            }
        }

        /* Anyadir un prologo al header que lea los parametros externos
         * desde registros VM (como si fueran parametros de funcion). */
        for (uint32_t vid : external_params) {
            /* Crear un valor parametro en la nueva funcion. */
            const ir::IrValueId new_param = out_body.new_value(
                source.values[vid].type,
                source.values[vid].name + "_osr_param");
            out_body.params.push_back(new_param);

            /* Insertar un MOV al inicio del header para mapear el
             * parametro al VID original.  En la practica, el regalloc
             * hara la coalescencia. */
            if (vid != new_param) {
                ir::IrInstr mov;
                mov.op = ir::IrOp::MOV;
                mov.type = source.values[vid].type;
                mov.dst = vid;  /* Usar el VID original */
                mov.operands = {new_param};
                /* Insertar al principio del header. */
                out_body.blocks[block_map[header_block]].instrs.insert(
                    out_body.blocks[block_map[header_block]].instrs.begin(),
                    std::move(mov));
            }
        }

        return true;
    }

    // =====================================================================
    //  Compilacion C1 (loop simple)
    // =====================================================================

    OsrCompileResult OsrCompiler::compile_c1(
        const ir::IrFunction &loop_body,
        const OsrCompileRequest &req) {
        OsrCompileResult result;

        /* Configurar opciones del selector con OSR entry. */
        SelectorOptions opts;
        opts.mode = SelectorMode::VM_ABI;
        opts.runtime = &rt_;
        if (rt_.safepoint_handler) {
            opts.safepoint_handler_addr =
                reinterpret_cast<uint64_t>(rt_.safepoint_handler);
        }

        /* Compilar la funcion del bucle con el compilador JIT. */
        JitCompiler compiler(cache_, rt_);
        CompileResult cres = compiler.compile_with_opts(loop_body, opts);
        if (!cres.fn) {
            return result;
        }

        /* Construir la entrada OSR. */
        OsrEntry entry;
        entry.compiled_address = reinterpret_cast<uint64_t>(cres.fn);
        entry.byte_offset      = 0;  /* C1: entry point al inicio del codigo */
        entry.loop_id          = req.loop_id;
        entry.bytecode_pc      = req.loop_header_pc;
        entry.function_name    = loop_body.name;
        entry.native_frame_size = 0; /* Lo rellenaria el selector si soporta */
        entry.valid            = true;

        /* Mapeo vid->buffer: en C1, todos los vids solicitados se
         * mapean secuencialmente. */
        entry.live_count = 0;
        for (uint32_t i = 0; i < req.live_vids.size()
             && i < OSR_MAX_LIVE_REGS; ++i) {
            entry.vid_to_buffer_slot[i] = static_cast<int16_t>(i);
            entry.live_count++;
        }

        result.osr_entry_address = entry.compiled_address;
        result.osr_entry_offset  = entry.byte_offset;
        result.entry             = entry;
        result.success           = true;

        return result;
    }

    // =====================================================================
    //  Compilacion C2 (loop optimizado)
    // =====================================================================

    OsrCompileResult OsrCompiler::compile_c2(
        const ir::IrFunction &loop_body,
        const OsrCompileRequest &req,
        std::function<uint64_t(const std::string &)> resolve_user_fn) {
        OsrCompileResult result;

        /* Para C2, intentamos primero el path de registros virtuales
         * (vreg_pipeline) que soporta OSR entry nativamente via
         * OsrEmit / rewrite_to_physical. */

        /* Construir VregEntries desde los runtime entries. */
        VregEntries ent;
        if (rt_.callvirt)            ent.callvirt  = reinterpret_cast<uint64_t>(rt_.callvirt);
        if (rt_.gc_deref)            ent.gc_deref  = reinterpret_cast<uint64_t>(rt_.gc_deref);
        if (rt_.gc_handle_for_ptr)   ent.gc_handle = reinterpret_cast<uint64_t>(rt_.gc_handle_for_ptr);
        if (rt_.gc_alloc)            ent.gc_allocp = reinterpret_cast<uint64_t>(rt_.gc_alloc);
        if (rt_.raw_alloc)           ent.raw_alloc = reinterpret_cast<uint64_t>(rt_.raw_alloc);
        if (rt_.raw_free)            ent.raw_free  = reinterpret_cast<uint64_t>(rt_.raw_free);

        /* Compilar por el path vreg con OSR entry.  vreg_compile_osr
         * (declarado en vreg_pipeline.h) produce dos punteros:
         * el codigo completo y el entry OSR. */
        uint8_t *osr_entry = nullptr;
        uint8_t *c2_code = vreg_compile_osr(
            loop_body, cache_, resolve_user_fn, ent,
            std::function<uint64_t(const std::string &)>{},
            std::function<uint64_t(const std::string &)>{},
            0,  /* header_block: el primer bloque es el loop header extraido */
            &osr_entry,
            &req.captured_vids);

        if (!c2_code || !osr_entry) {
            /* Fallback: intentar C1 si C2 falla. */
            return result;
        }

        /* Construir la entrada OSR con el entry point real. */
        OsrEntry entry;
        entry.compiled_address = reinterpret_cast<uint64_t>(osr_entry);
        entry.byte_offset      = 0;
        entry.loop_id          = req.loop_id;
        entry.bytecode_pc      = req.loop_header_pc;
        entry.function_name    = loop_body.name + "_c2";
        entry.live_count       = 0;
        for (uint32_t i = 0; i < req.live_vids.size()
             && i < OSR_MAX_LIVE_REGS; ++i) {
            entry.vid_to_buffer_slot[i] = static_cast<int16_t>(i);
            entry.live_count++;
        }
        entry.valid = true;

        result.osr_entry_address = entry.compiled_address;
        result.osr_entry_offset  = entry.byte_offset;
        result.entry             = entry;
        result.success           = true;

        return result;
    }

    // =====================================================================
    //  Compilacion principal
    // =====================================================================

    OsrCompileResult OsrCompiler::compile_loop(
        const ir::IrFunction &ir_fn,
        const OsrCompileRequest &req,
        OsrCompileTier tier,
        std::function<uint64_t(const std::string &)> resolve_user_fn) {

        OsrCompileResult result;

        /* Si el tier es INTERPRETED, no compilar. */
        if (tier == OsrCompileTier::C1_INTERPRETED) {
            return result;
        }

        /* Extraer el cuerpo del bucle como funcion independiente. */
        ir::IrFunction loop_body;
        if (!extract_loop_body(ir_fn, 0, loop_body)) {
            /* Si no se puede extraer, compilar la funcion completa
             * con un entry OSR en el offset 0 (fallback). */
            loop_body = ir_fn;
        }

        /* Compilar segun el tier. */
        switch (tier) {
            case OsrCompileTier::C1_LOOP:
                result = compile_c1(loop_body, req);
                break;
            case OsrCompileTier::C2_OPTIMIZED:
                result = compile_c2(loop_body, req, resolve_user_fn);
                /* Si C2 fallo, intentar C1. */
                if (!result.success) {
                    result = compile_c1(loop_body, req);
                }
                break;
            default:
                return result;
        }

        /* Si la compilacion fue exitosa, registrar la entrada. */
        if (result.success) {
            register_osr_entry(result.entry);
        }

        return result;
    }

    // =====================================================================
    //  Registro de entrada OSR
    // =====================================================================

    void OsrCompiler::register_osr_entry(const OsrEntry &entry) {
        OsrTable &table = get_osr_table_mutable();
        table.register_entry(entry);

        /* Actualizar el punto OSR correspondiente con la direccion
         * compilada para que el monitor pueda encontrarla. */
        for (auto &pt : table.points) {
            if (pt.loop_id == entry.loop_id) {
                pt.compiled_entry_offset = entry.byte_offset;
                break;
            }
        }
    }

} // namespace jit
