/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/interval.cpp
 * @brief Implementacion del constructor de live intervals (Phase D.7).
 *
 * Ver interval.h y doc/REGALLOC.md.  Algoritmo: gen/kill por bloque ->
 * liveness por dataflow iterativo a punto fijo -> construccion de rangos en
 * una pasada backward por bloque.  Optimizado: liveness con bitsets
 * word-packed (uniones/diferencias por palabra de 64 bits), rangos en
 * vectores planos.
 */

#include "jit/interval.h"

#include <algorithm>

namespace jit {

    /* ===================================================================== */
    /* operand_roles                                                          */
    /* ===================================================================== */

    InstrRoles operand_roles(MOp op) noexcept {
        using R = OperandRole;
        InstrRoles r;
        switch (op) {
            /* ALU binarios (forma 3-op pre-legalization): dst def, srcs use. */
            case MOp::ADD: case MOp::SUB: case MOp::IMUL:
            case MOp::AND: case MOp::OR:  case MOp::XOR:
            case MOp::SHL: case MOp::SHR: case MOp::SAR:
            case MOp::ROL: case MOp::ROR:
            case MOp::DIVMOD_V:   /* dst = src1 / src2 (variant: 0 DIV, 1 MOD) */
                r.dst = R::DEF; r.src1 = R::USE; r.src2 = R::USE; break;

            /* Unarios: dst def, src1 use. */
            case MOp::MOV:   case MOp::MOVZX: case MOp::MOVSX:
            case MOp::NEG:   case MOp::NOT:
            case MOp::INC:   case MOp::DEC:
            case MOp::BSWAP: case MOp::POPCNT: case MOp::LZCNT: case MOp::TZCNT:
                r.dst = R::DEF; r.src1 = R::USE; break;

            /* LEA dst, base, disp: dst def, base(src1) use, disp(src2) imm. */
            case MOp::LEA:
                r.dst = R::DEF; r.src1 = R::USE; break;

            /* SETcc dst: solo def (lee flags, no regs). */
            case MOp::SETCC:
                r.dst = R::DEF; break;

            /* CMOVcc dst, src: dst es use+def (condicional preserva), src use. */
            case MOp::CMOVCC:
                r.dst = R::USEDEF; r.src1 = R::USE; break;

            /* Comparaciones: solo leen (setean flags). */
            case MOp::CMP: case MOp::TEST: case MOp::UCOMISD:
                r.src1 = R::USE; r.src2 = R::USE; break;

            /* IDIV src: divisor use; dividendo/resultado en RAX/RDX (fijos,
             * constraints del allocator).  CQO no toca vregs. */
            case MOp::IDIV:
                r.src1 = R::USE; break;

            /* PUSH src / POP dst (prologue de callee-saved; raramente vregs). */
            case MOp::PUSH: r.src1 = R::USE; break;
            case MOp::POP:  r.dst  = R::DEF; break;

            /* ARG: marca un argumento de CALL; su vreg (src1) es un USE
             * (vivo hasta justo antes de la llamada). */
            case MOp::ARG:  r.src1 = R::USE; break;

            /* LOAD: dst = [addr].  STORE: [addr] = val (commit 7). */
            case MOp::LOAD:  r.dst = R::DEF; r.src1 = R::USE; break;
            case MOp::STORE: r.src1 = R::USE; r.src2 = R::USE; break;
            /* LOAD_VM/STORE_VM (vm_mem, 2026-06-09): igual rol que LOAD/STORE.
             * Los operandos imm64_idx (src2 en LOAD_VM, dst en STORE_VM) no son
             * vregs -> el interval builder los ignora via is_vreg(). */
            case MOp::LOAD_VM:  r.dst = R::DEF; r.src1 = R::USE; break;
            case MOp::STORE_VM: r.src1 = R::USE; r.src2 = R::USE; break;
            /* ALLOCA: dst = host_ptr al frame (src1 = size imm, no es vreg). */
            case MOp::ALLOCA: r.dst = R::DEF; break;
            /* ALLOCA_VM: dst = vaddr al VM stack (src1 = size imm).  Solo
             * mueve proc->stack_pointer (mov/sub/mov), NO hace CALL -> NO es
             * call-position. */
            case MOp::ALLOCA_VM: r.dst = R::DEF; break;

            /* FP scalar (XMM).  En v1 los XMM son fisicos hardcodeados (no
             * vregs), pero clasificamos por correccion para la fase FP. */
            case MOp::ADDSD: case MOp::SUBSD: case MOp::MULSD: case MOp::DIVSD:
            case MOp::MINSD: case MOp::MAXSD:
                r.dst = R::DEF; r.src1 = R::USE; r.src2 = R::USE; break;
            case MOp::SQRTSD: case MOp::ROUNDSD:
            case MOp::CVTSI2SD: case MOp::CVTTSD2SI:
            case MOp::CVTSS2SD: case MOp::CVTSD2SS:
            case MOp::MOVQ_GP_XMM: case MOp::MOVQ_XMM_GP:
                r.dst = R::DEF; r.src1 = R::USE; break;

            /* Control de flujo / pseudo: sin operandos de registro vreg. */
            default:
                break;
        }
        return r;
    }

    /* ===================================================================== */
    /* LiveInterval::add_range  (insert + coalesce)                           */
    /* ===================================================================== */

    void LiveInterval::add_range(uint32_t from, uint32_t to) {
        if (from >= to) return;  // rango vacio: nada que anyadir
        /* Caso comun (construccion backward): el nuevo rango toca o precede
         * al primer rango existente.  Coalesce con el front si solapan o son
         * adyacentes; si no, insercion ordenada general. */
        if (ranges.empty()) {
            ranges.push_back(LiveRange{from, to});
            return;
        }
        /* Fast path: extiende el primer rango si [from,to) lo toca por la
         * izquierda (lo mas frecuente en la pasada backward). */
        LiveRange &f = ranges.front();
        if (to >= f.from && from <= f.to) {
            f.from = std::min(f.from, from);
            f.to   = std::max(f.to, to);
            return;
        }
        if (to < f.from) {
            ranges.insert(ranges.begin(), LiveRange{from, to});
            return;
        }
        /* Camino general: insertar ordenado por @c from y coalescer. */
        size_t i = 0;
        while (i < ranges.size() && ranges[i].from < from) ++i;
        ranges.insert(ranges.begin() + i, LiveRange{from, to});
        /* Coalesce hacia adelante desde i-1. */
        size_t j = (i > 0) ? i - 1 : 0;
        while (j + 1 < ranges.size()) {
            if (ranges[j].to >= ranges[j + 1].from) {
                ranges[j].to = std::max(ranges[j].to, ranges[j + 1].to);
                ranges[j].from = std::min(ranges[j].from, ranges[j + 1].from);
                ranges.erase(ranges.begin() + j + 1);
            } else {
                ++j;
            }
        }
    }

    /* ===================================================================== */
    /* LiveInterval::first_overlap_from                                       */
    /* ===================================================================== */

    uint32_t LiveInterval::first_overlap_from(const LiveInterval &o,
                                              uint32_t pos) const noexcept {
        /* Dos pasadas sincronizadas sobre rangos ordenados (merge-like). */
        size_t i = 0, j = 0;
        while (i < ranges.size() && j < o.ranges.size()) {
            const LiveRange &a = ranges[i];
            const LiveRange &b = o.ranges[j];
            const uint32_t lo = std::max(std::max(a.from, b.from), pos);
            const uint32_t hi = std::min(a.to, b.to);
            if (lo < hi) return lo;          // solapan a partir de lo
            if (a.to <= b.to) ++i; else ++j; // avanzar el que termina antes
        }
        return UINT32_MAX;
    }

    /* ===================================================================== */
    /* build_intervals                                                        */
    /* ===================================================================== */

    namespace {

        /**
         * @brief Bitset word-packed minimo (vector<uint64_t>) para liveness.
         *        Operaciones por palabra de 64 bits para cache locality.
         */
        struct Bitset {
            std::vector<uint64_t> w;
            explicit Bitset(size_t nbits = 0) { resize(nbits); }
            void resize(size_t nbits) { w.assign((nbits + 63) / 64, 0); }
            void set(uint32_t i)   noexcept { w[i >> 6] |= (1ull << (i & 63)); }
            void clear(uint32_t i) noexcept { w[i >> 6] &= ~(1ull << (i & 63)); }
            bool test(uint32_t i) const noexcept {
                return (w[i >> 6] >> (i & 63)) & 1ull;
            }
            /** @brief this |= o.  Devuelve true si cambio algun bit. */
            bool union_with(const Bitset &o) noexcept {
                bool changed = false;
                for (size_t k = 0; k < w.size(); ++k) {
                    const uint64_t before = w[k];
                    w[k] |= o.w[k];
                    changed |= (w[k] != before);
                }
                return changed;
            }
            /** @brief Copia o y resta kill: this = src & ~kill. */
            void assign_sub(const Bitset &src, const Bitset &kill) noexcept {
                for (size_t k = 0; k < w.size(); ++k) w[k] = src.w[k] & ~kill.w[k];
            }
            void union_inplace(const Bitset &o) noexcept {
                for (size_t k = 0; k < w.size(); ++k) w[k] |= o.w[k];
            }
            bool equals(const Bitset &o) const noexcept { return w == o.w; }
            void copy_from(const Bitset &o) noexcept { w = o.w; }
        };

    } // namespace

    IntervalResult build_intervals(const MFunction &mf, const TargetRegInfo &tri) {
        (void)tri;  // commit 2: el target se usara para constraints en commit 4
        IntervalResult out;
        const uint32_t NV = mf.vreg_count;
        const size_t NB = mf.blocks.size();

        out.intervals.resize(NV);
        for (uint32_t v = 0; v < NV; ++v) {
            out.intervals[v].vreg = v;
            out.intervals[v].cls  = (v < mf.vreg_class.size())
                                        ? mf.vreg_class[v] : RegClass::GP;
            /* Phase D.7 commit 6: propagar la categoria GC (kind+1). */
            out.intervals[v].gc_kind = (v < mf.vreg_is_gc.size())
                                        ? mf.vreg_is_gc[v] : 0;
        }
        if (NV == 0 || NB == 0) return out;

        /* ---- 1) Posiciones lineales por instruccion (use=par, def=impar) ----
         * @c first_gi[b] = indice global de la primera instr del bloque b.
         * @c block_start/@c block_end en espacio de posiciones (2 por instr). */
        std::vector<uint32_t> first_gi(NB, 0);
        std::vector<uint32_t> block_start(NB, 0), block_end(NB, 0);
        {
            uint32_t gi = 0;
            for (size_t b = 0; b < NB; ++b) {
                first_gi[b]    = gi;
                block_start[b] = 2u * gi;
                gi += static_cast<uint32_t>(mf.blocks[b].instrs.size());
                block_end[b]   = 2u * gi;
            }
            out.max_pos = 2u * gi;
        }

        /* Helper: invoca @p fn(vreg_id, role) por cada operando VREG de la
         * instr, segun los roles del opcode. */
        auto each_vreg = [](const MInstr &in, auto &&fn) {
            const InstrRoles roles = operand_roles(in.op);
            if (in.dst.is_vreg()  && roles.dst  != OperandRole::NONE)
                fn(in.dst.vreg_id(),  roles.dst);
            if (in.src1.is_vreg() && roles.src1 != OperandRole::NONE)
                fn(in.src1.vreg_id(), roles.src1);
            if (in.src2.is_vreg() && roles.src2 != OperandRole::NONE)
                fn(in.src2.vreg_id(), roles.src2);
        };

        /* ---- 2) gen/kill por bloque ---- */
        std::vector<Bitset> gen(NB, Bitset(NV)), kill(NB, Bitset(NV));
        for (size_t b = 0; b < NB; ++b) {
            const auto &blk = mf.blocks[b];
            for (const MInstr &in : blk.instrs) {
                if (in.op == MOp::CALL || in.op == MOp::CALL_ABS) {
                    /* posiciones de call: relleno mas abajo (necesito gi). */
                }
                each_vreg(in, [&](uint32_t v, OperandRole role) {
                    const bool is_use = (role == OperandRole::USE ||
                                         role == OperandRole::USEDEF);
                    const bool is_def = (role == OperandRole::DEF ||
                                         role == OperandRole::USEDEF);
                    /* gen = uso ANTES de def en el bloque (upward-exposed). */
                    if (is_use && !kill[b].test(v)) gen[b].set(v);
                    if (is_def) kill[b].set(v);
                });
            }
        }

        /* Posiciones de CALL (para clobbers del allocator, commit 4). */
        for (size_t b = 0; b < NB; ++b) {
            uint32_t gi = first_gi[b];
            for (const MInstr &in : mf.blocks[b].instrs) {
                /* DIVMOD_V clobbea RAX/RDX (idiv) -> tratarlo como call-position
                 * para que los vregs vivos a traves vayan a callee-saved.
                 * LOAD_VM/STORE_VM: su page-miss hace CALL a vrt_vm_read/write
                 * -> clobbea caller-saved -> tambien call-position. */
                if (in.op == MOp::CALL || in.op == MOp::CALL_ABS
                 || in.op == MOp::DIVMOD_V
                 || in.op == MOp::LOAD_VM || in.op == MOp::STORE_VM)
                    out.call_positions.push_back(2u * gi);
                ++gi;
            }
        }

        /* ---- 3) Liveness por dataflow iterativo a punto fijo ----
         * live_out[b] = U live_in[succ];  live_in[b] = gen[b] U (live_out − kill).
         * Iterar en orden REVERSO de layout converge rapido para CFGs
         * reducibles (lo normal en codigo Vex). */
        std::vector<Bitset> live_in(NB, Bitset(NV)), live_out(NB, Bitset(NV));
        bool changed = true;
        /* Scratch reusado entre iteraciones (cero alocaciones en el loop). */
        Bitset tmp_out(NV), new_in(NV);
        while (changed) {
            changed = false;
            for (size_t bi = NB; bi-- > 0;) {
                const auto &blk = mf.blocks[bi];
                /* new_out = union de live_in de sucesores. */
                std::fill(tmp_out.w.begin(), tmp_out.w.end(), 0ull);
                if (blk.succ_a != MBLOCK_INVALID)
                    tmp_out.union_inplace(live_in[blk.succ_a]);
                if (blk.succ_b != MBLOCK_INVALID && blk.succ_b != blk.succ_a)
                    tmp_out.union_inplace(live_in[blk.succ_b]);
                if (!tmp_out.equals(live_out[bi])) {
                    live_out[bi].copy_from(tmp_out);
                    changed = true;
                }
                /* new_in = gen U (out & ~kill). */
                new_in.assign_sub(live_out[bi], kill[bi]);
                new_in.union_inplace(gen[bi]);
                if (!new_in.equals(live_in[bi])) {
                    live_in[bi].copy_from(new_in);
                    changed = true;
                }
            }
        }

        /* ---- 4) Construccion de rangos POR BLOQUE via live-in/out ----
         * Para cada bloque y cada vreg, se calcula su rango EN ese bloque a
         * partir de @c live_in/@c live_out (ya computados) + su primera
         * aparicion (def/use) y ultimo uso dentro del bloque.  Esto maneja
         * correctamente el MULTI-DEF que produce la eliminacion de PHI (un
         * vreg PHI se define en cada predecesor): el rango de un valor
         * loop-carried cubre TODO el loop (live-in && live-out -> bloque
         * completo) sin que un def en un bloque corte el rango de otro -- a
         * diferencia del set_from ingenuo, que asumia SSA single-def. */
        std::vector<uint32_t> b_first(NV, UINT32_MAX), b_first_def(NV, UINT32_MAX),
                              b_last_use(NV, 0);
        std::vector<uint8_t>  b_used(NV, 0), b_def(NV, 0);
        std::vector<uint32_t> touched;
        touched.reserve(64);
        for (size_t b = 0; b < NB; ++b) {
            const auto &blk = mf.blocks[b];
            const uint32_t bstart = block_start[b];
            const uint32_t bend   = block_end[b];

            /* Reset de los temporales tocados en el bloque anterior. */
            for (uint32_t v : touched) {
                b_first[v] = UINT32_MAX; b_first_def[v] = UINT32_MAX;
                b_last_use[v] = 0; b_used[v] = 0; b_def[v] = 0;
            }
            touched.clear();

            /* Escaneo forward: primera aparicion, primer def, ultimo uso +
             * acumular posiciones de uso (ascendente). */
            const uint32_t base_gi = first_gi[b];
            for (size_t j = 0; j < blk.instrs.size(); ++j) {
                const MInstr &in = blk.instrs[j];
                const uint32_t gi = base_gi + static_cast<uint32_t>(j);
                const uint32_t use_pos = 2u * gi;
                const uint32_t def_pos = 2u * gi + 1u;
                each_vreg(in, [&](uint32_t v, OperandRole role) {
                    if (b_first[v] == UINT32_MAX) touched.push_back(v);
                    if (role == OperandRole::USE || role == OperandRole::USEDEF) {
                        b_used[v] = 1; b_last_use[v] = use_pos;
                        if (use_pos < b_first[v]) b_first[v] = use_pos;
                        out.intervals[v].uses.push_back(use_pos);
                    }
                    if (role == OperandRole::DEF || role == OperandRole::USEDEF) {
                        b_def[v] = 1;
                        if (def_pos < b_first_def[v]) b_first_def[v] = def_pos;
                        if (def_pos < b_first[v]) b_first[v] = def_pos;
                    }
                });
            }

            /* Construir el rango de cada vreg relevante en este bloque. */
            for (uint32_t v = 0; v < NV; ++v) {
                const bool in  = live_in[b].test(v);
                const bool out_ = live_out[b].test(v);
                const bool appears = (b_first[v] != UINT32_MAX);
                if (!in && !out_ && !appears) continue;
                const uint32_t start = in ? bstart
                                          : (appears ? b_first[v] : bstart);
                uint32_t end;
                if (out_)            end = bend;            // vivo al salir
                else if (b_used[v])  end = b_last_use[v] + 1u;  // muere en su ultimo uso
                else if (b_def[v])   end = b_first_def[v] + 1u; // def muerto
                else                 end = bend;            // live-in raro: conservador
                out.intervals[v].add_range(start, end);
            }
        }
        return out;
    }

} // namespace jit
