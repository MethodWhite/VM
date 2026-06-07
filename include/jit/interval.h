/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/interval.h
 * @brief Construccion de live intervals sobre MachineIR para el register
 *        allocator (Phase D.7, commit 2).  Ver doc/REGALLOC.md.
 *
 * Un *live interval* describe, para cada registro virtual, EN QUE POSICIONES
 * lineales del codigo esta vivo (puede tener "holes": muere y revive).  Es la
 * entrada del linear-scan (commit 4): el allocator recorre los intervalos por
 * orden de inicio y asigna fisicos / spillea segun solapen o no.
 *
 * = Posiciones lineales =
 *
 * Cada instruccion ocupa DOS posiciones: @c use (par) y @c def (impar).  La
 * i-esima instruccion (orden global de layout) tiene @c use_pos = 2*i y
 * @c def_pos = 2*i+1.  Asi un valor USADO y RE-DEFINIDO en la misma instr no
 * solapa consigo mismo (su uso termina en use_pos, el nuevo def empieza en
 * def_pos) -- imprescindible para que dst pueda reusar el reg de un src
 * (two-address).
 *
 * = Liveness =
 *
 * Dataflow iterativo a punto fijo (gen/kill por bloque, live-in/out), que
 * maneja back-edges (loops) de forma robusta sin necesidad de deteccion de
 * loops.  Tras la liveness, los rangos se construyen en una pasada backward
 * por bloque usando @c live_out como conjunto vivo al final del bloque.  La
 * correccion NO depende del orden de bloques (solo la calidad del spilling).
 *
 * = Roles de operandos =
 *
 * El unico punto consciente del opcode: @c operand_roles(MOp) dice si cada
 * uno de @c {dst, src1, src2} es def, use, ambos o nada, asumiendo la forma
 * de TRES operandos pre-legalization (el dst de un ALU es def puro; la
 * two-address legalization es posterior).  En un futuro multi-arch esta
 * tabla se mueve a la capa del selector/target; hoy @c MOp es compartido.
 */

#ifndef VESTA_JIT_INTERVAL_H
#define VESTA_JIT_INTERVAL_H

#include "jit/machine_ir.h"
#include "jit/target_reginfo.h"

#include <cstdint>
#include <vector>

namespace jit {

    /**
     * @struct LiveRange
     * @brief Rango semi-abierto [from, to) de posiciones lineales donde un
     *        vreg esta vivo.
     */
    struct LiveRange {
        uint32_t from = 0;  ///< posicion de inicio (inclusive)
        uint32_t to   = 0;  ///< posicion de fin (exclusive)
    };

    /**
     * @struct LiveInterval
     * @brief Conjunto de rangos vivos de UN registro virtual, mas sus
     *        posiciones de uso (heuristica de spill).
     *
     * @c ranges queda ordenado ascendente y SIN solapes tras la
     * construccion.  Un interval vacio (@c ranges vacio) = vreg sin usos
     * (codigo muerto que el allocator ignora).
     */
    struct LiveInterval {
        uint32_t vreg = UINT32_MAX;          ///< id del registro virtual
        RegClass cls  = RegClass::GP;         ///< clase (GP/FP)
        std::vector<LiveRange> ranges;        ///< rangos vivos, ordenados/disjuntos
        std::vector<uint32_t>  uses;          ///< posiciones de uso (ascendente)
        /// Phase D.7 commit 6: categoria GC.  0 = no GC; 1+ = StackmapGcKind+1
        /// (1=HANDLE, 2=HOSTPTR, 3=STRING).  Si != 0 y el intervalo cruza un
        /// call, el allocator lo FUERZA a un slot (enfoque A) para que el GC
        /// lo describa via stackmap y lo escanee del stack.
        uint8_t  gc_kind = 0;

        bool is_gc() const noexcept { return gc_kind != 0; }
        bool empty() const noexcept { return ranges.empty(); }

        /** @brief Primera posicion viva (o UINT32_MAX si vacio). */
        uint32_t start() const noexcept {
            return ranges.empty() ? UINT32_MAX : ranges.front().from;
        }
        /** @brief Posicion fin (exclusive) del ultimo rango (o 0 si vacio). */
        uint32_t end() const noexcept {
            return ranges.empty() ? 0u : ranges.back().to;
        }

        /** @brief True si @p pos cae dentro de algun rango vivo. */
        bool covers(uint32_t pos) const noexcept {
            /* ranges ordenados: busqueda lineal (pocos rangos por vreg). */
            for (const auto &r : ranges) {
                if (pos < r.from) return false;     // ranges ascendentes
                if (pos < r.to)   return true;
            }
            return false;
        }

        /**
         * @brief Primera posicion >= @p pos en la que dos intervalos estan
         *        vivos a la vez, o UINT32_MAX si nunca solapan en o despues
         *        de @p pos.  Lo usa el allocator para detectar interferencia.
         */
        uint32_t first_overlap_from(const LiveInterval &o,
                                    uint32_t pos) const noexcept;

        /** @brief Primera posicion de uso > @p pos (o UINT32_MAX). */
        uint32_t next_use_after(uint32_t pos) const noexcept {
            for (uint32_t u : uses) if (u > pos) return u;
            return UINT32_MAX;
        }

        /**
         * @brief Anyade un rango [from, to).  Coalesce con rangos
         *        existentes solapados/adyacentes manteniendo orden.
         */
        void add_range(uint32_t from, uint32_t to);

        /**
         * @brief Acorta el inicio del PRIMER rango a @p pos (un def: el vreg
         *        no esta vivo antes de su definicion en este bloque).
         */
        void set_from(uint32_t pos) noexcept {
            if (!ranges.empty()) ranges.front().from = pos;
        }
    };

    /**
     * @struct IntervalResult
     * @brief Salida de @c build_intervals.
     */
    struct IntervalResult {
        /// Indexado por vreg id (denso 0..vreg_count-1).  Intervals vacios
        /// = vregs sin uso.
        std::vector<LiveInterval> intervals;
        /// Posiciones de cada CALL (use_pos).  El allocator (commit 4) las
        /// usa para clobbear caller-saved e insertar save/restore.
        std::vector<uint32_t> call_positions;
        /// Posicion lineal maxima + 1 (tamano del espacio de posiciones).
        uint32_t max_pos = 0;
    };

    /**
     * @enum OperandRole
     * @brief Rol de un operando respecto al dataflow.
     */
    enum class OperandRole : uint8_t {
        NONE   = 0,  ///< no es un registro relevante (label, imm, vacio)
        USE    = 1,  ///< se lee
        DEF    = 2,  ///< se escribe
        USEDEF = 3   ///< se lee y se escribe (p.ej. CMOVcc condicional)
    };

    /**
     * @struct InstrRoles
     * @brief Roles de @c {dst, src1, src2} para una MInstr.
     */
    struct InstrRoles {
        OperandRole dst  = OperandRole::NONE;
        OperandRole src1 = OperandRole::NONE;
        OperandRole src2 = OperandRole::NONE;
    };

    /**
     * @brief Clasifica los roles de los operandos de un opcode (forma de
     *        tres operandos pre-legalization).
     */
    InstrRoles operand_roles(MOp op) noexcept;

    /**
     * @brief Construye los live intervals de los vregs de @p mf.
     *
     * @param mf   Funcion en MachineIR (forma vreg).
     * @param tri  Descriptor del target (para futuras constraints; en commit
     *             2 se usa solo para validacion).
     * @return     Intervals por vreg + posiciones de CALL.
     */
    IntervalResult build_intervals(const MFunction &mf, const TargetRegInfo &tri);

} // namespace jit

#endif // VESTA_JIT_INTERVAL_H
