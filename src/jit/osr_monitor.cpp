/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_monitor.cpp
 * @brief Monitor de bucles para OSR: cuenta back-edges y dispara
 *        compilacion OSR cuando se supera el umbral.
 *
 * = Flujo =
 *
 * 1. El interprete llama a @c osr_monitor_backedge cada vez que ejecuta
 *    un back-edge (salto hacia atras en el bytecode).
 *
 * 2. El monitor incrementa el contador asociado al punto OSR (loop header).
 *
 * 3. Cuando el contador supera OSR_THRESHOLD (10,000 por defecto):
 *    a. Captura el estado del interprete via @c capture_osr_state.
 *    b. Solicita la compilacion OSR via @c OsrCompiler::compile_loop.
 *    c. Si la compilacion tiene exito, inicia la transferencia
 *       (osr_transfer).
 *
 * 4. Si la compilacion falla (no soportada), se marca el punto como
 *    no-OSReable y no se vuelve a intentar.
 *
 * = Concurrencia =
 *
 * El monitor se ejecuta en el hilo del interprete (single-thread por
 * proceso en v1).  No hay races: el contador se lee/escribe desde el
 * mismo hilo.  La compilacion JIT ocurre bajo @c g_compile_mtx.
 */

#include "jit/osr_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "ir/ssa_ir.h"
#include "jit/jit_compiler.h"
#include "jit/osr_compiler.h"
#include "jit/osr_data.h"
#include "jit/osr_transfer.h"
#include "runtime/proceso_runtime.h"
#include "vesta_rt/abi.h"
#include "vesta_rt/public.h"

namespace jit {

    // =====================================================================
    //  Constantes y estado global del monitor
    // =====================================================================

    /// Umbral de back-edges para disparar OSR.
    static uint32_t g_osr_threshold = OSR_DEFAULT_THRESHOLD;

    /// True si el monitor OSR esta habilitado.
    static bool g_osr_enabled = true;

    /// Mapa: loop_id -> contador de back-edges.
    static std::unordered_map<uint32_t, uint32_t> g_osr_backedge_counts;

    /// Mapa: loop_id -> true si ya se intento compilar (exito o fallo).
    static std::unordered_map<uint32_t, bool> g_osr_compile_attempted;

    /// Tabla OSR global (puntos + entradas).
    static OsrTable g_osr_table;

    /// Referencia al compilador JIT (seteado por init_osr_monitor).
    static OsrCompiler *g_osr_compiler = nullptr;

    /// Referencia al code cache (seteado por init_osr_monitor).
    static CodeCache *g_osr_code_cache = nullptr;

    /// Referencia a los runtime entries.
    static const RuntimeEntries *g_osr_rt = nullptr;

    // =====================================================================
    //  OsrTable implementation (forward-declarada en osr_data.h)
    // =====================================================================

    uint32_t OsrTable::find_or_create_point(uint64_t bytecode_pc,
                                            uint32_t func_hash) {
        for (uint32_t i = 0; i < points.size(); ++i) {
            if (points[i].bytecode_pc == bytecode_pc
             && points[i].function_hash == func_hash) {
                return i;
            }
        }
        if (points.size() >= OSR_MAX_POINTS_PER_FUNCTION) {
            return UINT32_MAX;
        }
        OsrPoint pt;
        pt.bytecode_pc   = bytecode_pc;
        pt.function_hash = func_hash;
        pt.loop_id       = next_loop_id++;
        points.push_back(pt);
        return static_cast<uint32_t>(points.size() - 1);
    }

    const OsrEntry *OsrTable::find_entry(uint32_t loop_id) const {
        for (const auto &e : entries) {
            if (e.loop_id == loop_id) return &e;
        }
        return nullptr;
    }

    void OsrTable::register_entry(const OsrEntry &entry) {
        for (auto &e : entries) {
            if (e.loop_id == entry.loop_id) {
                e = entry;
                return;
            }
        }
        entries.push_back(entry);
    }

    // =====================================================================
    //  Initializacion
    // =====================================================================

    void init_osr_monitor(CodeCache &cache, const RuntimeEntries &rt) noexcept {
        g_osr_code_cache = &cache;
        g_osr_rt         = &rt;
        if (!g_osr_compiler) {
            g_osr_compiler = new OsrCompiler(cache, rt);
        }
        /* Leer override del threshold desde entorno. */
        const char *env = std::getenv("VESTA_OSR_THRESHOLD");
        if (env && env[0] != '\0') {
            char *end = nullptr;
            const unsigned long v = std::strtoul(env, &end, 10);
            if (end != env && v <= UINT32_MAX) {
                g_osr_threshold = static_cast<uint32_t>(v);
            }
        }
        const char *dis = std::getenv("VESTA_OSR_DISABLE");
        if (dis && dis[0] != '\0' && dis[0] != '0') {
            g_osr_enabled = false;
        }
    }

    void set_osr_threshold(uint32_t threshold) noexcept {
        g_osr_threshold = threshold;
    }

    bool osr_enabled() noexcept {
        return g_osr_enabled;
    }

    // =====================================================================
    //  Back-edge monitoring
    // =====================================================================

    uint32_t osr_monitor_backedge(uint64_t bytecode_pc,
                                  uint64_t function_vaddr,
                                  uint32_t function_hash) noexcept {
        if (!g_osr_enabled) return 0;

        /* Encontrar o crear el punto OSR para este loop header. */
        const uint32_t pt_idx = g_osr_table.find_or_create_point(
            bytecode_pc, function_hash);
        if (pt_idx == UINT32_MAX) return 0;

        OsrPoint &pt = g_osr_table.points[pt_idx];
        pt.back_edge_count++;

        /* Si el contador no supera el umbral, no hacer nada. */
        if (pt.back_edge_count < g_osr_threshold) {
            return pt.back_edge_count;
        }

        /* Si ya se intento compilar (exito o fallo), no reintentar. */
        if (g_osr_compile_attempted[pt.loop_id]) {
            return pt.back_edge_count;
        }
        g_osr_compile_attempted[pt.loop_id] = true;

        /* Disparar la compilacion OSR.  Esto se hace sincronicamente
         * desde el hilo del interprete.  En una version futura se podria
         * delegar a un thread de compilacion en background. */
        trigger_osr_compilation(pt, function_vaddr);

        return pt.back_edge_count;
    }

    // =====================================================================
    //  Compilation trigger
    // =====================================================================

    OsrState capture_osr_state(vrt_proc *proc,
                               uint64_t bytecode_pc,
                               uint32_t loop_id) noexcept {
        OsrState state;
        state.bytecode_pc = bytecode_pc;
        state.loop_id     = loop_id;

        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);
        if (!p) return state;

        /* Capturar registros VM. */
        for (int i = 0; i < VESTA_PROC_REGISTER_COUNT; ++i) {
            state.regs[i] = p->registers.regs[i].qword();
        }
        state.stack_pointer = p->registers.stack_pointer.qword();
        state.base_pointer  = p->registers.base_pointer.qword();

        /* Capturar el tope de la pila (unos pocos slots para
         * rematerializar valores).  No capturamos toda la pila por
         * eficiencia; solo los slots que el codigo compilado necesita. */
        const uint32_t capture_depth = 16;
        state.stack_depth = capture_depth;
        state.stack_slots.reserve(capture_depth);
        uint64_t sp = state.stack_pointer;
        for (uint32_t i = 0; i < capture_depth; ++i) {
            try {
                state.stack_slots.push_back(
                    p->vm_mem.read_u64(sp + i * 8));
            } catch (...) {
                state.stack_slots.push_back(0);
            }
        }

        return state;
    }

    bool trigger_osr_compilation(OsrPoint &pt,
                                 uint64_t function_vaddr) noexcept {
        if (!g_osr_compiler || !g_osr_code_cache) return false;

        /* Construir la solicitud de compilacion. */
        OsrCompileRequest req;
        req.function_vaddr = function_vaddr;
        req.loop_header_pc = pt.bytecode_pc;
        req.loop_id        = pt.loop_id;
        req.back_edge_count = pt.back_edge_count;
        req.live_vids      = pt.live_vids;

        /* Seleccionar el tier segun la hotness del bucle.  Como
         * regla simple: bucles con muchas iteraciones reciben C2. */
        const OsrCompileTier tier = OsrCompiler::select_tier(
            pt.back_edge_count);

        /* En v1 no tenemos acceso directo al ir::IrFunction desde el
         * monitor en runtime (el monitor vive en el interprete y el IR
         * esta en el Loader).  La compilacion OSR real se delega a
         * auto_jit.cpp que tiene acceso al IR.  Aqui marcamos el punto
         * como listo para que auto_jit lo procese.
         *
         * El flag compiled_entry_offset != 0 indica que hay codigo
         * compilado disponible; el transfer lo consulta. */

        /* Por ahora, en OSR Phase D.5, el monitor solo registra el
         * punto como candidato.  La compilacion real ocurre en
         * eager_compile_function (auto_jit.cpp) cuando el C1 detecta
         * loops via regalloc_rewrite.  Aqui seteamos compiled_entry_offset
         * a un valor no-cero cuando el compilador externo lo rellene. */
        if (pt.compiled_entry_offset != 0) {
            return true;
        }

        return false;
    }

    // =====================================================================
    //  Lookup helpers
    // =====================================================================

    uint64_t osr_lookup_entry(uint32_t loop_id) noexcept {
        const OsrEntry *entry = g_osr_table.find_entry(loop_id);
        if (entry && entry->valid) {
            return entry->compiled_address;
        }
        return 0;
    }

    const OsrTable &get_osr_table() noexcept {
        return g_osr_table;
    }

    OsrTable &get_osr_table_mutable() noexcept {
        return g_osr_table;
    }

} // namespace jit
