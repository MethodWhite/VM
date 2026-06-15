/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/regalloc_rewrite.cpp
 * @brief Implementacion del rewrite vreg -> fisico (Phase D.7, commit 4a).
 *        Ver regalloc_rewrite.h y doc/REGALLOC.md.
 */

#include "jit/regalloc_rewrite.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "vesta_rt/abi.h"  // VESTA_PROC_OSR_BUFFER_OFFSET / VESTA_OSR_BUFFER_N

namespace jit {

    namespace {

        /** @brief Diagnostico/A-B: VESTA_JIT_NO_FRAMELESS=1 fuerza el frame
         *  completo (push rbp / mov rbp,rsp / pop rbp) incluso en hojas que
         *  calificarian para frameless.  Sirve para medir el efecto del
         *  frameless o aislar un posible bug del codegen del prologo/epilogo
         *  (el codegen mas safety-critical: un fallo aqui corrompe el heap via
         *  el precise stack-scan de la GC). */
        bool jit_no_frameless() noexcept {
            static const bool off = []{
                const char *v = std::getenv("VESTA_JIT_NO_FRAMELESS");
                return v && v[0] != '\0' && v[0] != '0';
            }();
            return off;
        }

        /** @brief OSR 1a (diagnostico, VESTA_OSR_COUNT=1): instrumenta cada
         *  back-edge con un contador de iteraciones.  Emitido POST-regalloc
         *  (aqui, no en vreg_select) para no pasar por la legalizacion 2-address
         *  de @c lower() -- que mangla @c ADD [mem],imm al ser ADD un bin_alu.
         *  El encoder procesa la secuencia verbatim (igual que el contador
         *  on-entry del Selector).  Prerrequisito del tier-up por back-edge (1b)
         *  y del OSR real (2). */
        bool jit_osr_count() noexcept {
            static const bool on = []{
                const char *v = std::getenv("VESTA_OSR_COUNT");
                return v && v[0] != '\0' && v[0] != '0';
            }();
            return on;
        }
        /* Contador unico global de iteraciones de back-edge (diagnostico 1a). */
        uint64_t g_osr_be_total = 0;
        uint32_t g_osr_be_sites = 0;

        /** @brief Umbral de iteraciones para disparar el tier-up por back-edge
         *  (1b).  Default 100k; override VESTA_OSR_THRESHOLD. */
        uint32_t jit_osr_threshold() noexcept {
            static const uint32_t t = []{
                const char *v = std::getenv("VESTA_OSR_THRESHOLD");
                return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10))
                         : 100000u;
            }();
            return t;
        }
        /* Cuantas veces se disparo el trigger (diagnostico). */
        uint64_t g_osr_trigger_hits = 0;

        /* ---- OSR paso 2a: descriptor del estado capturado por loop ---- */

        /** @brief Una celda del state-transfer: el IR VID y si es raiz GC.
         *  El valor vive en @c osr_buffer[vid] tras la captura del trigger. */
        struct OsrCaptureSlot {
            uint32_t vid;    ///< IR value id (== indice en osr_buffer)
            uint8_t  is_gc;  ///< 1 si es host_ptr/handle a objeto GC
        };
        /** @brief Descriptor de un loop OSR-instrumentado, indexado por loop_id.
         *  Construido en compile-time (rewrite_to_physical); lo consume el
         *  handler para loguear (2a) y, en 2b/2c, dirigir el salto C1->C2. */
        struct OsrLoopDesc {
            std::string                 fn_name;       ///< nombre de la funcion
            uint32_t                    header_block;  ///< MBlock id del loop header
            std::vector<OsrCaptureSlot> captures;      ///< live-in del header
            bool                        aborted;       ///< true si no se capturo estado
        };
        /* Indexado por loop_id (== el contador g_osr_be_sites del sitio). */
        std::vector<OsrLoopDesc> g_osr_loops;

        /** @brief Handler de OSR instalado por auto_jit (2c).  Dado un loop_id,
         *  devuelve la direccion del OSR-entry del C2 precompilado (o 0 si no
         *  hay variante).  nullptr por defecto -> el trigger solo loguea y el
         *  C1 NO hace swap (comportamiento 2a/2b). */
        uint64_t (*g_osr_handler)(uint64_t) = nullptr;

        /** @brief Diagnostico opt-in del trigger (VESTA_OSR_LOG=1): loguea el
         *  estado capturado en cada disparo.  Off por defecto para no spamear
         *  cuando el OSR real (swap) ya esta activo. */
        bool jit_osr_log() noexcept {
            static const bool on = []{
                const char *v = std::getenv("VESTA_OSR_LOG");
                return v && v[0] != '\0' && v[0] != '0';
            }();
            return on;
        }

        /** @brief Stub del trigger OSR.  Se invoca UNA vez por back-edge cuando
         *  el contador iguala el umbral.  Recibe el buffer con el estado del loop
         *  capturado por el C1 (puede loguearlo con VESTA_OSR_LOG=1).  Devuelve
         *  la direccion del OSR-entry del C2 (via @c g_osr_handler) para que el
         *  C1 haga el frame-swap; 0 = no swap (el C1 continua su loop).
         *  Convencion C: (proc, loop_id, buffer) -> uint64_t en RAX. */
        uint64_t osr_trigger_stub(void * /*proc*/, uint64_t loop_id,
                                  uint64_t *buffer) {
            ++g_osr_trigger_hits;
            if (jit_osr_log()) {
                if (loop_id < g_osr_loops.size()) {
                    const OsrLoopDesc &d =
                        g_osr_loops[static_cast<size_t>(loop_id)];
                    std::fprintf(stderr,
                        "[osr] TRIGGER loop_id=%llu fn=%s header_bb=%u capturas=%zu%s\n",
                        static_cast<unsigned long long>(loop_id),
                        d.fn_name.c_str(), d.header_block, d.captures.size(),
                        d.aborted ? " (ABORTADO: estado no capturable)" : "");
                    if (buffer && !d.aborted) {
                        for (const OsrCaptureSlot &c : d.captures) {
                            const uint64_t val = buffer[c.vid];
                            std::fprintf(stderr,
                                "[osr]   v%u%s = %lld (0x%llx)\n", c.vid,
                                c.is_gc ? " gc" : "",
                                static_cast<long long>(val),
                                static_cast<unsigned long long>(val));
                        }
                    }
                } else {
                    std::fprintf(stderr,
                        "[osr] TRIGGER loop_id=%llu (umbral %u)\n",
                        static_cast<unsigned long long>(loop_id),
                        jit_osr_threshold());
                }
            }
            /* 2c: delegar en el handler para obtener el OSR-entry del C2. */
            return g_osr_handler ? g_osr_handler(loop_id) : 0;
        }
        /** @brief Registra (una vez) el dump atexit del contador de back-edges. */
        void osr_install_dump_once() {
            static bool installed = false;
            if (installed) return;
            installed = true;
            std::atexit([]{
                std::fprintf(stderr,
                    "[osr] back-edges=%u  iteraciones_totales=%llu  triggers=%llu\n",
                    g_osr_be_sites,
                    static_cast<unsigned long long>(g_osr_be_total),
                    static_cast<unsigned long long>(g_osr_trigger_hits));
            });
        }

        /** @brief True si la ALU binaria @p op es conmutativa. */
        bool is_commutative(MOp op) noexcept {
            switch (op) {
                case MOp::ADD: case MOp::AND:
                case MOp::OR:  case MOp::XOR:
                case MOp::IMUL: return true;
                default:        return false;
            }
        }

        /** @brief True si @p op es una ALU binaria que reescribimos (4a). */
        bool is_bin_alu(MOp op) noexcept {
            switch (op) {
                case MOp::ADD: case MOp::SUB: case MOp::AND:
                case MOp::OR:  case MOp::XOR: case MOp::IMUL: return true;
                default:        return false;
            }
        }

        /**
         * @struct Lowerer
         * @brief Estado del rewrite de una funcion.
         */
        struct Lowerer {
            const RegAlloc      &ra;
            const TargetRegInfo &tri;
            bool     vm_abi = false;  ///< VM_ABI (salva RBX=ProcessVM*) vs host leaf
            bool     no_frame = false;///< hoja frameless: sin push/mov rbp ni sub rsp
            uint32_t k = 0;          ///< numero de callee-saved asignados
            uint32_t total_saved = 0;///< callee-saved + (vm_abi ? 1 (rbx) : 0)
            int32_t  spill_bytes = 0;///< tamano del area de spills (alineado)
            /// Commit 8: offset (desde RBP) del inicio del area de allocas
            /// (justo debajo de los spill slots) + cursor de asignacion.
            uint32_t alloca_base = 0;
            uint32_t alloca_cursor = 0;
            /// Fase 2 (ALLOCA_VM): la fn reserva en el VM stack del proceso.
            /// Hay que salvar el VM-RSP (proc->stack_pointer) al entry y
            /// restaurarlo en CADA RET, o el VM stack hace leak/overflow entre
            /// llamadas.  @c vm_rsp_save_off es el offset (negativo desde RBP)
            /// del slot que guarda el VM-RSP original (un qword debajo del area
            /// de allocas host, por encima del shadow space Win64).
            bool     has_vm_alloca = false;
            int32_t  vm_rsp_save_off = 0;
            MReg     scr0 = MReg::R10;
            MReg     scr1 = MReg::R11;
            /// MFunction destino: necesario para crear labels intra-expansion
            /// (LOAD_VM/STORE_VM page-cache) via @c pf->new_label().  Se asigna
            /// en @c rewrite_to_physical tras construir pf.
            MFunction *pf = nullptr;

            Lowerer(const RegAlloc &r, const TargetRegInfo &t, AbiKind abi,
                    bool has_calls, uint32_t alloca_total, bool has_vm_alloca_in)
                : ra(r), tri(t), vm_abi(abi == AbiKind::VM),
                  has_vm_alloca(has_vm_alloca_in) {
                k = static_cast<uint32_t>(ra.callee_saved_used.size());
                total_saved = k + (vm_abi ? 1u : 0u);  // +1 por el push rbx
                /* Hoja frameless: una funcion sin CALLs que no spillea ni
                 * reserva allocas no necesita frame pointer.  RBP solo
                 * direcciona spill slots y el area de allocas; sin ellos se
                 * omite todo el frame (push rbp / mov rbp,rsp / sub rsp / lea /
                 * pop rbp).  Es SEGURO respecto a la GC: el precise stack-scan
                 * camina la cadena RBP, pero solo en un SAFEPOINT, y una hoja
                 * no tiene safepoint (no llama a nada) -> ningun frame de hoja
                 * esta vivo cuando corre la GC.  Aun en VM_ABI se conserva el
                 * push/pop de RBX (callee-saved del host que trae ProcessVM*) y
                 * los callee-saved usados; lo unico que desaparece es RBP. */
                no_frame = !has_calls && ra.num_spill_slots == 0u &&
                           alloca_total == 0u && !has_vm_alloca
                           && !jit_no_frameless()
                           && !jit_osr_count();  /* el trigger (1b) anyade un
                              call -> necesita frame con rsp 16-alineado. */
                /* Las allocas viven debajo de los spill slots. */
                alloca_base = 8u * total_saved + 8u * ra.num_spill_slots;
                spill_bytes = static_cast<int32_t>(
                    8u * ra.num_spill_slots + alloca_total);
                /* Fase 2: reservar un qword para el VM-RSP salvado, debajo del
                 * area de allocas host y por encima del shadow space.  El
                 * offset es fijo desde RBP (independiente del shadow/align que
                 * se anyade despues, que solo crece el frame hacia abajo). */
                if (has_vm_alloca) {
                    vm_rsp_save_off = -static_cast<int32_t>(
                        8u * total_saved + 8u * ra.num_spill_slots
                        + alloca_total + 8u);
                    spill_bytes += 8;
                }
                if (!no_frame) {
#if defined(_WIN32)
                    /* Win64: si hay CALLs, reservar 32 bytes de shadow/home
                     * space en el FONDO del frame (debajo de los spill slots)
                     * para que el callee no pise nuestros datos. */
                    if (has_calls) spill_bytes += 32;
#else
                    (void)has_calls;
#endif
                    /* Alinear (8*total_saved + spill_bytes) a 16 para mantener
                     * el stack 16-aligned en CALLs internos. */
                    if (((8u * total_saved) +
                         static_cast<uint32_t>(spill_bytes)) % 16u != 0u)
                        spill_bytes += 8;
                }
                /* Frameless: spill_bytes queda en 0 (no hay spills ni allocas) y
                 * no se alinea a 16 ni se reserva shadow space porque no hay
                 * CALLs internos. */
                const auto &sc = tri.scratch[static_cast<size_t>(RegClass::GP)];
                if (sc.size() >= 1) scr0 = static_cast<MReg>(sc[0]);
                if (sc.size() >= 2) scr1 = static_cast<MReg>(sc[1]);
            }

            /// Argumentos pendientes (idx, ubicacion) de la proxima CALL.
            std::vector<std::pair<uint8_t, MOperand>> pending_args;

            /// Phase D.7 commit 6: intervalos (para stackmaps en CALLs) +
            /// posicion lineal del CALL actual + stackmaps acumulados.
            const IntervalResult *ivs = nullptr;
            uint32_t cur_call_pos = 0;
            std::vector<Stackmap> stackmaps;

            /**
             * @brief Emite un PARALLEL MOVE: el conjunto de copias
             *        @c dst_reg <- src se realiza simultaneamente.  Emite en
             *        un orden seguro y rompe ciclos con @p scratch.
             */
            void emit_parallel_moves(
                std::vector<std::pair<MReg, MOperand>> moves,
                MReg scratch, std::vector<MInstr> &out) const {
                const size_t n = moves.size();
                std::vector<bool> done(n, false);
                size_t remaining = n;
                auto reads_dst = [&](size_t i) -> bool {
                    for (size_t j = 0; j < n; ++j) {
                        if (done[j] || j == i) continue;
                        if (moves[j].second.kind == MOperandKind::REG &&
                            moves[j].second.reg == reg_id(moves[i].first))
                            return true;
                    }
                    return false;
                };
                while (remaining > 0) {
                    bool progress = false;
                    for (size_t i = 0; i < n; ++i) {
                        if (done[i] || reads_dst(i)) continue;
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            reg(moves[i].first), moves[i].second));
                        done[i] = true; --remaining; progress = true;
                    }
                    if (progress) continue;
                    /* Ciclo: salvar un dst a scratch y romperlo. */
                    for (size_t i = 0; i < n; ++i) {
                        if (done[i]) continue;
                        const MReg d = moves[i].first;
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scratch), reg(d)));
                        for (size_t j = 0; j < n; ++j)
                            if (!done[j] && moves[j].second.kind == MOperandKind::REG &&
                                moves[j].second.reg == reg_id(d))
                                moves[j].second = reg(scratch);
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(d), moves[i].second));
                        done[i] = true; --remaining;
                        break;
                    }
                }
            }

            /** @brief Registro host que trae el @c ProcessVM* (primer arg). */
            static MReg proc_arg_reg() noexcept {
#if defined(_WIN32)
                return MReg::RCX;
#else
                return MReg::RDI;
#endif
            }

            /** @brief Offset desde RBP del spill slot @p slot. */
            int32_t slot_off(uint32_t slot) const noexcept {
                return -static_cast<int32_t>(8u * total_saved + 8u * (slot + 1u));
            }

            /** @brief Operando de memoria del spill slot @p slot: [rbp+off]. */
            MOperand slot_mem(uint32_t slot) const noexcept {
                return MOperand::make_mem(MReg::RBP, slot_off(slot));
            }

            /** @brief Resuelve un operando vreg a su ubicacion fisica. */
            MOperand resolve(const MOperand &o) const noexcept {
                if (!o.is_vreg()) return o;  // imm/label/mem/reg fisico: passthrough
                const uint32_t vid = o.vreg_id();
                if (ra.in_reg(vid))
                    return MOperand::make_reg(static_cast<MReg>(ra.reg_of(vid)), o.width);
                return slot_mem(ra.slot_of(vid));  // spilled
            }

            static MOperand reg(MReg r) { return MOperand::make_reg(r, 8); }
            static MInstr push(MReg r) {
                MInstr i; i.op = MOp::PUSH; i.src1 = reg(r); return i;
            }
            static MInstr pop(MReg r) {
                MInstr i; i.op = MOp::POP; i.dst = reg(r); return i;
            }

            void emit_prologue(std::vector<MInstr> &out) const {
                if (!no_frame) {
                    out.push_back(push(MReg::RBP));
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RBP),
                                                     reg(MReg::RSP)));
                }
                if (vm_abi) {
                    /* Salvar RBX (callee-saved del host) y cargarlo con el
                     * ProcessVM* que llega en el primer arg. */
                    out.push_back(push(MReg::RBX));
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RBX),
                                                     reg(proc_arg_reg())));
                }
                for (uint8_t r : ra.callee_saved_used)
                    out.push_back(push(static_cast<MReg>(r)));
                if (spill_bytes > 0)
                    out.push_back(MInstr::make_unary(MOp::SUB, reg(MReg::RSP),
                                                     MOperand::make_imm32(spill_bytes)));
                /* Fase 2: salvar el VM-RSP original al slot del frame.  Los
                 * ALLOCA_VM mas adelante decrementan proc->stack_pointer; el
                 * epilogue lo restaura desde aqui (si no, el VM stack hace
                 * leak/overflow entre llamadas).  scr0 (R10) es caller-saved y
                 * esta libre aqui; RBX ya trae el ProcessVM* (vm_abi). */
                if (has_vm_alloca) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0),
                        MOperand::make_mem(MReg::RBX,
                            VESTA_PROC_STACK_POINTER_OFFSET)));
                    out.push_back(MInstr::make_unary(MOp::MOV,
                        MOperand::make_mem(MReg::RBP, vm_rsp_save_off),
                        reg(scr0)));
                }
            }

            void emit_epilogue(std::vector<MInstr> &out) const {
                if (no_frame) {
                    /* Frameless: rsp ya apunta justo encima de los registros
                     * salvados (no hubo push rbp ni sub rsp).  Solo se deshacen
                     * los push de callee-saved y RBX en orden inverso; el ret
                     * encuentra la return address exactamente. */
                    for (size_t i = ra.callee_saved_used.size(); i-- > 0;)
                        out.push_back(pop(static_cast<MReg>(ra.callee_saved_used[i])));
                    if (vm_abi) out.push_back(pop(MReg::RBX));
                    return;
                }
                /* Fase 2: restaurar el VM-RSP original ANTES de desmontar el
                 * frame (RBP/RBX aun validos).  Sin esto los ALLOCA_VM dejarian
                 * proc->stack_pointer decrementado tras el RET -> leak/overflow
                 * del VM stack global.  scr0 (R10) es caller-saved (libre). */
                if (has_vm_alloca) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0),
                        MOperand::make_mem(MReg::RBP, vm_rsp_save_off)));
                    out.push_back(MInstr::make_unary(MOp::MOV,
                        MOperand::make_mem(MReg::RBX,
                            VESTA_PROC_STACK_POINTER_OFFSET),
                        reg(scr0)));
                }
                /* lea rsp, [rbp - 8*total_saved] -> deshace el sub del frame y
                 * apunta rsp al ultimo registro salvado. */
                out.push_back(MInstr::make_unary(
                    MOp::LEA, reg(MReg::RSP),
                    MOperand::make_mem(MReg::RBP,
                        -static_cast<int32_t>(8u * total_saved))));
                /* pop callee en orden inverso. */
                for (size_t i = ra.callee_saved_used.size(); i-- > 0;)
                    out.push_back(pop(static_cast<MReg>(ra.callee_saved_used[i])));
                if (vm_abi) out.push_back(pop(MReg::RBX));
                out.push_back(pop(MReg::RBP));
            }

            /** @brief Reescribe una instr vreg a 0+ instrs fisicas. */
            void lower(const MInstr &in, std::vector<MInstr> &out) {
                const MOp op = in.op;

                if (op == MOp::ARG) {
                    /* Acumular: (indice, ubicacion fisica del vreg arg). */
                    pending_args.emplace_back(in.variant, resolve(in.src1));
                    return;
                }

                if (op == MOp::CALL_ABS) {
                    /* Parallel-move de los args a los registros de argumento
                     * del ABI host, luego mov scratch,addr + call scratch. */
                    const auto &areg = tri.arg_regs[static_cast<size_t>(RegClass::GP)];
                    std::vector<std::pair<MReg, MOperand>> moves;
                    for (const auto &pa : pending_args) {
                        if (pa.first < areg.size())
                            moves.emplace_back(static_cast<MReg>(areg[pa.first]), pa.second);
                        /* args >12 / en stack: no soportado v1 (raro). */
                    }
                    emit_parallel_moves(std::move(moves), scr1, out);
                    pending_args.clear();
                    /* Cargar la direccion (imm64 del pool) en scratch y llamar. */
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), in.src1));
                    MInstr call; call.op = MOp::CALL; call.src1 = reg(scr0);
                    /* Stackmap (commit 6): describir los GC roots vivos a
                     * traves de este call.  El linear_scan los forzo a slots,
                     * asi que estan en el stack -> el GC los lee via stackmap.
                     * Se asocia al CALL por @c flags (idx); el encoder rellena
                     * el pc_offset. */
                    if (ivs != nullptr) {
                        Stackmap sm;
                        const uint32_t NVI =
                            static_cast<uint32_t>(ivs->intervals.size());
                        for (uint32_t v = 0; v < NVI; ++v) {
                            const LiveInterval &lv = ivs->intervals[v];
                            if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                            if (!ra.spilled(v)) continue;  // invariante: GC+cross-call -> slot
                            StackmapSlot s;
                            s.rbp_offset = static_cast<int16_t>(slot_off(ra.slot_of(v)));
                            s.gc_kind = static_cast<StackmapGcKind>(
                                static_cast<uint8_t>(lv.gc_kind - 1u));
                            sm.slots.push_back(s);
                        }
                        call.flags = static_cast<uint16_t>(stackmaps.size());
                        stackmaps.push_back(std::move(sm));
                    }
                    out.push_back(call);
                    return;
                }

                if (op == MOp::CALL) {
                    /* CALL INDIRECTO (call reg/mem): el target es un vreg (p.ej.
                     * el jit_code resuelto en el inline dispatch de CALLVIRT).
                     * No hay ARG marshalling (los args ya estan en
                     * proc->registers).  Igual que CALL_ABS, emite el stackmap
                     * de los GC roots vivos a traves de este call para que el GC
                     * los escanee desde el stack (el commit 6 los forzo a slot). */
                    MOperand tgt = resolve(in.src1);
                    if (tgt.kind == MOperandKind::MEM) {
                        /* target spilled -> cargar a scratch antes del call. */
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), tgt));
                        tgt = reg(scr0);
                    }
                    MInstr call; call.op = MOp::CALL; call.src1 = tgt;
                    if (ivs != nullptr) {
                        Stackmap sm;
                        const uint32_t NVI =
                            static_cast<uint32_t>(ivs->intervals.size());
                        for (uint32_t v = 0; v < NVI; ++v) {
                            const LiveInterval &lv = ivs->intervals[v];
                            if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                            if (!ra.spilled(v)) continue;
                            StackmapSlot s;
                            s.rbp_offset = static_cast<int16_t>(slot_off(ra.slot_of(v)));
                            s.gc_kind = static_cast<StackmapGcKind>(
                                static_cast<uint8_t>(lv.gc_kind - 1u));
                            sm.slots.push_back(s);
                        }
                        call.flags = static_cast<uint16_t>(stackmaps.size());
                        stackmaps.push_back(std::move(sm));
                    }
                    out.push_back(call);
                    return;
                }

                if (op == MOp::TAILCALL) {
                    /* TCO con reuso de frame (mismo patron que el frame-swap del
                     * OSR 2c): proc -> A0 (sobrevive el teardown por ser
                     * caller-saved; el prologue del callee hace mov rbx,A0) +
                     * emit_epilogue (desmonta el frame; rsp queda en la return
                     * address del caller) + jmp al target.  El RET del callee
                     * retorna al caller original -> pila O(1).  No hay GC entre
                     * el teardown y la reentrada (sin safepoint/call) -> los args
                     * en proc->registers (ya escritos) siguen como roots. */
                    const auto &areg =
                        tri.arg_regs[static_cast<size_t>(RegClass::GP)];
                    if (vm_abi && !areg.empty())
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            reg(static_cast<MReg>(areg[0])), reg(MReg::RBX)));
                    emit_epilogue(out);
                    if (in.src1.kind == MOperandKind::LABEL) {
                        /* self-tail-call: jmp rel32 a code+0 (label bloque 0). */
                        MInstr j; j.op = MOp::JMP; j.src1 = in.src1;
                        out.push_back(j);
                    } else {
                        /* cross-fn: mov scr0, addr(imm64) + jmp scr0. */
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0),
                                                         in.src1));
                        MInstr j; j.op = MOp::JMP; j.src1 = reg(scr0);
                        out.push_back(j);
                    }
                    return;
                }

                if (op == MOp::RET) {
                    emit_epilogue(out);
                    out.push_back(MInstr::make_ret());
                    return;
                }

                if (op == MOp::DIVMOD_V) {
                    /* dst = src1 / src2 (variant 0) o src1 % src2 (variant 1),
                     * signed.  Secuencia x86 con RAX:RDX fijos.  El divisor va a
                     * R11 (scr1, reservado -> nunca es un operando, sin aliasing)
                     * ANTES de tocar RAX/RDX, asi cualquier asignacion fisica de
                     * los operandos (incluida RAX/RDX) es segura:
                     *   mov  r11, divisor      ; divisor leido antes de clobbers
                     *   mov  rax, dividendo    ; (no-op si ya en RAX)
                     *   cqo                    ; sign-extend RAX -> RDX:RAX
                     *   idiv r11               ; RAX=cociente, RDX=resto
                     *   mov  dst, rax|rdx
                     * DIVMOD_V es call-position -> ningun vreg vivo a traves esta
                     * en RAX/RDX (van a callee-saved), asi el clobber es seguro. */
                    const MOperand a = resolve(in.src1);   // dividendo
                    const MOperand b = resolve(in.src2);   // divisor
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), b));
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RAX), a));
                    { MInstr c; c.op = MOp::CQO; out.push_back(c); }
                    out.push_back(MInstr::make_unary(MOp::IDIV, MOperand{},
                                                     reg(scr1)));
                    const MReg res = (in.variant == 1u) ? MReg::RDX : MReg::RAX;
                    out.push_back(MInstr::make_unary(MOp::MOV,
                                                     resolve(in.dst), reg(res)));
                    return;
                }

                if (op == MOp::LOAD) {
                    /* dst = [addr].  addr y dst pueden estar spilled. */
                    const uint8_t width = static_cast<uint8_t>(in.flags >> 1);
                    const bool    sgn   = (in.flags & 1u) != 0u;
                    MOperand a = resolve(in.src1);
                    MReg addr_reg;
                    if (a.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                        addr_reg = scr1;
                    } else {
                        addr_reg = static_cast<MReg>(a.reg);
                    }
                    /* NO tocar mem.width: empaqueta scale|index del MEM.  El
                     * ancho de un MOV [mem] lo da el reg; el de MOVSX/MOVZX va
                     * en mem.flags (mem_size override). */
                    MOperand mem = MOperand::make_mem(addr_reg, 0);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    if (width == 8) {
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, mem));
                    } else if (sgn) {
                        mem.flags = width;  // ancho del src para MOVSX
                        out.push_back(MInstr::make_unary(MOp::MOVSX, pdst, mem));
                    } else {  // u8/u16 (u32 lo filtro el selector)
                        mem.flags = width;
                        out.push_back(MInstr::make_unary(MOp::MOVZX, pdst, mem));
                    }
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::STORE) {
                    /* [addr] = val.  addr -> scr1 si spilled; val -> scr0 si spilled. */
                    const uint8_t width = static_cast<uint8_t>(in.flags);
                    MOperand a = resolve(in.src1);
                    MReg addr_reg;
                    if (a.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                        addr_reg = scr1;
                    } else {
                        addr_reg = static_cast<MReg>(a.reg);
                    }
                    MOperand v = resolve(in.src2);
                    if (v.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), v));
                        v = reg(scr0);
                    }
                    v.width = width;  // ancho del store lo da el reg src
                    /* NO tocar mem.width (index packing). */
                    MOperand mem = MOperand::make_mem(addr_reg, 0);
                    out.push_back(MInstr::make_unary(MOp::MOV, mem, v));
                    return;
                }

                if (op == MOp::LOAD_VM || op == MOp::STORE_VM) {
                    /* Acceso a memoria del VM (vaddr, NO host_ptr).  La memoria
                     * del VM no es contigua (TLB 3-niveles) -> page-cache INLINE
                     * de 1 entrada (replica de selector.cpp): si la pagina del
                     * vaddr coincide con la cacheada en proc->vm_mem y no cruza
                     * el limite, traduce inline (~hit ~8 instr SIN call); si no,
                     * CALL a vrt_vm_read/write_u<w>.  scr0/scr1 (R10/R11, reserva-
                     * dos) son los temporales del hit; areg/val se re-leen de su
                     * ubicacion canonica (resolve) en el miss -> intactos.  Es
                     * call-position (el miss clobbea caller-saved) -> los vregs
                     * vivos a traves ya estan en callee-saved/spill.
                     * NOTA: vrt_vm_read/write NO disparan GC (solo traducen) ->
                     * el CALL del miss no es safepoint -> sin stackmap. */
                    const bool    is_store = (op == MOp::STORE_VM);
                    const uint8_t width    = is_store
                        ? static_cast<uint8_t>(in.flags)
                        : static_cast<uint8_t>(in.flags >> 1);
                    const bool    sgn      = !is_store && ((in.flags & 1u) != 0u);
                    /* imm64_idx con la direccion de vrt_vm_read/write_u<w>:
                     * src2 en LOAD_VM, dst en STORE_VM (operandos libres). */
                    const MOperand fn_imm = is_store ? in.dst : in.src2;
                    const MOperand areg   = resolve(in.src1);   // vaddr (canonica)
                    const MOperand vval   = is_store ? resolve(in.src2)
                                                     : MOperand{};
                    /* dst del LOAD: reg fisico o scr0 si spilled. */
                    const bool dst_spilled = !is_store && in.dst.is_vreg()
                                          && ra.spilled(in.dst.vreg_id());
                    MReg pr = MReg::RAX;
                    if (!is_store)
                        pr = dst_spilled ? scr0
                           : static_cast<MReg>(resolve(in.dst).reg);

                    const bool inline_ok =
                        vesta_rt::kProcVmMemOffset != 0
                     && vesta_rt::kVmMemCachedPageVaddrOffset >= 0
                     && vesta_rt::kVmMemCachedPageHostOffset >= 0;
                    const int32_t page_v = vesta_rt::kProcVmMemOffset
                                         + vesta_rt::kVmMemCachedPageVaddrOffset;
                    const int32_t page_h = vesta_rt::kProcVmMemOffset
                                         + vesta_rt::kVmMemCachedPageHostOffset;
#if defined(_WIN32)
                    const MReg A0 = MReg::RCX, A1 = MReg::RDX, A2 = MReg::R8;
#else
                    const MReg A0 = MReg::RDI, A1 = MReg::RSI, A2 = MReg::RDX;
#endif
                    const MLabelId Lmiss = inline_ok ? pf->new_label()
                                                     : MLABEL_INVALID;
                    const MLabelId Ldone = inline_ok ? pf->new_label()
                                                     : MLABEL_INVALID;

                    if (inline_ok) {
                        /* --- HIT path --- */
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), areg));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), reg(scr1)));
                        out.push_back(MInstr::make_unary(MOp::AND, reg(scr0),
                            MOperand::make_imm32(-4096)));
                        out.push_back(MInstr::make_unary(MOp::CMP, reg(scr0),
                            MOperand::make_mem(MReg::RBX, page_v)));
                        out.push_back(MInstr::make_jcc(MCond::NE, Lmiss));
                        out.push_back(MInstr::make_unary(MOp::AND, reg(scr1),
                            MOperand::make_imm32(4095)));   // scr1 = offset
                        if (width > 1) {                    // cross-page check
                            out.push_back(MInstr::make_unary(MOp::CMP, reg(scr1),
                                MOperand::make_imm32(4096 -
                                    static_cast<int32_t>(width))));
                            out.push_back(MInstr::make_jcc(MCond::A, Lmiss));
                        }
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0),
                            MOperand::make_mem(MReg::RBX, page_h)));  // cached_host
                        out.push_back(MInstr::make_unary(MOp::ADD, reg(scr0),
                            reg(scr1)));                    // scr0 = host_ptr
                        if (is_store) {
                            MOperand v = vval;
                            if (v.kind == MOperandKind::MEM) {
                                out.push_back(MInstr::make_unary(MOp::MOV,
                                    reg(scr1), v));
                                v = reg(scr1);
                            }
                            v.width = width;                // ancho lo da el reg
                            out.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(scr0, 0), v));
                        } else {
                            MOperand mem = MOperand::make_mem(scr0, 0);
                            if (width >= 8) {
                                out.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(pr, 8), mem));
                            } else if (sgn) {
                                mem.flags = width;          // mem_size override
                                out.push_back(MInstr::make_unary(MOp::MOVSX,
                                    MOperand::make_reg(pr, 8), mem));
                            } else if (width == 4) {
                                out.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(pr, 4), mem));  // zero-ext
                            } else {
                                mem.flags = width;
                                out.push_back(MInstr::make_unary(MOp::MOVZX,
                                    MOperand::make_reg(pr, 8), mem));
                            }
                        }
                        out.push_back(MInstr::make_jmp(Ldone));
                        out.push_back(MInstr::make_label_def(Lmiss));
                    }

                    /* --- FALLBACK CALL (page-miss) ---
                     * Cargo vaddr (y val) a scratch ANTES de los arg-movs para
                     * no depender del orden de los arg_regs (robusto ante
                     * cualquier ubicacion fisica de areg/vval). */
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), areg));
                    if (is_store) {
                        /* val a scr0 (reg o mem, mov reg,* lo cubre). */
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), vval));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(A0), reg(MReg::RBX)));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(A1), reg(scr1)));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(A2), reg(scr0)));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), fn_imm));
                        MInstr call; call.op = MOp::CALL; call.src1 = reg(scr0);
                        out.push_back(call);
                    } else {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(A0), reg(MReg::RBX)));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(A1), reg(scr1)));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), fn_imm));
                        MInstr call; call.op = MOp::CALL; call.src1 = reg(scr0);
                        out.push_back(call);
                        /* resultado en RAX -> pr (igual que el selector). */
                        if (sgn && width < 8) {
                            MOperand src = MOperand::make_reg(MReg::RAX, width);
                            out.push_back(MInstr::make_unary(MOp::MOVSX,
                                MOperand::make_reg(pr, 8), src));
                        } else {
                            out.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(pr, 8), reg(MReg::RAX)));
                        }
                    }

                    if (inline_ok)
                        out.push_back(MInstr::make_label_def(Ldone));
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), reg(scr0)));
                    return;
                }

                if (op == MOp::ALLOCA) {
                    /* dst = host_ptr a `size` bytes reservados en el frame
                     * (debajo de los spills).  LEA dst, [rbp - off]. */
                    const uint32_t size =
                        static_cast<uint32_t>(in.src1.value);
                    const uint32_t aligned = (size + 7u) & ~7u;
                    const int32_t off = static_cast<int32_t>(
                        alloca_base + alloca_cursor + aligned);
                    alloca_cursor += aligned;
                    const MOperand mem = MOperand::make_mem(MReg::RBP, -off);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    out.push_back(MInstr::make_unary(MOp::LEA, pdst, mem));
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::ALLOCA_VM) {
                    /* dst = vaddr a `size` bytes del VM stack del proceso:
                     *   mov scr0, [rbx+SP]; sub scr0, aligned; mov [rbx+SP],scr0
                     *   dst = scr0.
                     * scr0 (R10) es scratch reservado; RBX trae el ProcessVM*.
                     * El prologue ya salvo el VM-RSP original y el epilogue lo
                     * restaura, asi que la resta es local al frame. */
                    const uint32_t size =
                        static_cast<uint32_t>(in.src1.value);
                    const uint32_t aligned = (size + 15u) & ~15u;  // 16-align
                    const MOperand sp_mem = MOperand::make_mem(
                        MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET);
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sp_mem));
                    if (aligned > 0)
                        out.push_back(MInstr::make_unary(MOp::SUB, reg(scr0),
                            MOperand::make_imm32(static_cast<int32_t>(aligned))));
                    out.push_back(MInstr::make_unary(MOp::MOV, sp_mem, reg(scr0)));
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), reg(scr0)));
                    else
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            resolve(in.dst), reg(scr0)));
                    return;
                }

                if (op == MOp::MOV) {
                    const MOperand rs = resolve(in.src1);
                    if (in.dst.is_vreg() && ra.spilled(in.dst.vreg_id())) {
                        const uint32_t slot = ra.slot_of(in.dst.vreg_id());
                        if (rs.kind == MOperandKind::REG) {
                            out.push_back(MInstr::make_unary(MOp::MOV, slot_mem(slot), rs));
                        } else {
                            /* mem<-mem o mem<-imm: pasar por scratch. */
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                            out.push_back(MInstr::make_unary(MOp::MOV, slot_mem(slot), reg(scr0)));
                        }
                    } else {
                        /* dst no-spilled (reg) o dst MEM fisico (p.ej. el
                         * return VM a [rbx+off]).  Si AMBOS son MEM (dst MEM
                         * fisico + src spilled), pasar por scratch (no hay
                         * mov mem,mem en x86). */
                        const MOperand d = resolve(in.dst);
                        if (d.kind == MOperandKind::MEM && rs.kind == MOperandKind::MEM) {
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                            out.push_back(MInstr::make_unary(MOp::MOV, d, reg(scr0)));
                        } else {
                            out.push_back(MInstr::make_unary(MOp::MOV, d, rs));
                        }
                    }
                    return;
                }

                if (op == MOp::MOVZX || op == MOp::MOVSX) {
                    /* Extension: dst64 <- src<width>.  El encoder exige
                     * dst=REG; si el src esta spilled, cargarlo a scratch1
                     * preservando su width; si el dst esta spilled, extender
                     * a scratch0 y almacenar. */
                    MOperand rs = resolve(in.src1);
                    if (rs.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs));
                        rs = MOperand::make_reg(scr1, in.src1.width);  // width del src
                    }
                    const bool dst_spilled = in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    MInstr ext; ext.op = op; ext.dst = pdst; ext.src1 = rs;
                    out.push_back(ext);
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::SHL || op == MOp::SHR || op == MOp::SAR
                 || op == MOp::ROL || op == MOp::ROR) {
                    /* dst = src1 (sh/rot) src2(imm).  MOV dst, src1; SHL dst, imm.
                     * Solo cantidad INMEDIATA (el selector emite ROL/ROR aqui solo
                     * con count constante; variable -> fallback, requiere CL). */
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    const MOperand rs1 = resolve(in.src1);
                    if (!(pdst.kind == MOperandKind::REG &&
                          rs1.kind == MOperandKind::REG && pdst.reg == rs1.reg))
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
                    MInstr sh; sh.op = op; sh.dst = pdst; sh.src1 = in.src2;  // imm
                    out.push_back(sh);
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::LZCNT || op == MOp::TZCNT || op == MOp::POPCNT) {
                    /* dst = op(src).  dst debe ser REG (si spilled -> scratch
                     * + store).  src puede ser MEM (op r64, r/m64). */
                    const MOperand rs = resolve(in.src1);
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    out.push_back(MInstr::make_unary(op, pdst, rs));
                    if (dst_spilled)
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    return;
                }

                if (op == MOp::BSWAP) {
                    /* bswap r64: in-place, dst debe ser REG.  Si dst spilled,
                     * cargar a scratch, bswap, store.  src1 == dst (mismo vreg). */
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    if (dst_spilled) {
                        const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                        out.push_back(MInstr::make_unary(MOp::BSWAP,
                            reg(scr0), reg(scr0)));
                        out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
                    } else {
                        const MOperand pdst = resolve(in.dst);
                        out.push_back(MInstr::make_unary(MOp::BSWAP, pdst, pdst));
                    }
                    return;
                }

                if (op == MOp::CMOVCC) {
                    /* dst = (cc) ? src : dst  (read-modify).  dst debe ser REG.
                     * Preserva variant (codigo de condicion).  src spilled -> scr1;
                     * dst spilled -> carga a scr0, cmov, store. */
                    MOperand rsrc = resolve(in.src1);
                    if (rsrc.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rsrc));
                        rsrc = reg(scr1);
                    }
                    const bool dst_spilled =
                        in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    MInstr c; c.op = MOp::CMOVCC; c.variant = in.variant;
                    if (dst_spilled) {
                        const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                        c.dst = reg(scr0); c.src1 = rsrc;
                        out.push_back(c);
                        out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
                    } else {
                        c.dst = resolve(in.dst); c.src1 = rsrc;
                        out.push_back(c);
                    }
                    return;
                }

                if (is_bin_alu(op)) {
                    const bool dst_spilled = in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
                    const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
                    const MOperand rs1  = resolve(in.src1);
                    const MOperand rs2  = resolve(in.src2);

                    const bool anti = (rs2.kind == MOperandKind::REG &&
                                       pdst.kind == MOperandKind::REG &&
                                       rs2.reg == pdst.reg);
                    /* IMUL (emit_imul) SOLO soporta reg,reg y reg,reg,imm -- NO
                     * acepta un operando fuente en memoria (a diferencia de
                     * ADD/SUB/AND/OR/XOR, cuyo emit_alu si maneja r/m).  Si el
                     * operando fuente del imul de 2 operandos esta spilled
                     * (MEM), cargarlo a scr1 primero.  Sin esto, un imul con un
                     * src spilled (p.ej. una constante loop-invariante que el
                     * regalloc spilleo bajo presion) caia al fallback 0xCC del
                     * encoder -> SIGTRAP en runtime. */
                    auto imul_fix = [&](MOperand src) -> MOperand {
                        if (op == MOp::IMUL && src.kind == MOperandKind::MEM) {
                            out.push_back(MInstr::make_unary(MOp::MOV,
                                                             reg(scr1), src));
                            return reg(scr1);
                        }
                        return src;
                    };
                    if (anti) {
                        if (is_commutative(op)) {
                            /* pdst ya contiene src2 -> OP pdst, src1 (conmutativo). */
                            out.push_back(MInstr::make_unary(op, pdst,
                                                             imul_fix(rs1)));
                        } else {
                            /* SUB y pdst==src2reg: usar scratch1 para src2. */
                            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs2));
                            out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
                            out.push_back(MInstr::make_unary(op, pdst, reg(scr1)));
                        }
                    } else {
                        out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1)); // pdst = src1
                        out.push_back(MInstr::make_unary(op, pdst,
                                                         imul_fix(rs2)));       // pdst OP= src2
                    }
                    if (dst_spilled) {
                        out.push_back(MInstr::make_unary(MOp::MOV,
                            slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                    }
                    return;
                }

                if (op == MOp::CMP || op == MOp::TEST) {
                    MOperand a = resolve(in.src1);
                    MOperand b = resolve(in.src2);
                    if (a.kind == MOperandKind::MEM && b.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), a));
                        a = reg(scr0);
                    }
                    out.push_back(MInstr::make_unary(op, a, b));  // dst=a, src1=b
                    return;
                }

                /* Resto (JMP/JCC/LABEL_DEF/NOP/...): sustituir vregs en sitio. */
                MInstr m = in;
                m.dst  = resolve(in.dst);
                m.src1 = resolve(in.src1);
                m.src2 = resolve(in.src2);
                out.push_back(m);
            }
        };

    } // namespace

    MFunction rewrite_to_physical(const MFunction &vf,
                                  const RegAlloc &ra,
                                  const TargetRegInfo &tri,
                                  AbiKind abi,
                                  const IntervalResult *ivs,
                                  OsrEmit *osr) {
        /* Detectar si la funcion tiene CALLs (para reservar shadow space). */
        bool has_calls = false;
        uint32_t alloca_total = 0;  // commit 8: bytes de allocas en el frame
        bool has_vm_alloca = false; // Fase 2: reserva en el VM stack del proceso
        for (const auto &b : vf.blocks) {
            for (const auto &in : b.instrs) {
                if (in.op == MOp::CALL || in.op == MOp::CALL_ABS) has_calls = true;
                /* LOAD_VM/STORE_VM: el page-miss emite un CALL a vrt_vm_*; aunque
                 * el hit no llama, debe reservarse el frame + shadow space Win64
                 * (no frameless). */
                if (in.op == MOp::LOAD_VM || in.op == MOp::STORE_VM) has_calls = true;
                /* TAILCALL: su expansion hace emit_epilogue (necesita el frame
                 * montado: lea rsp,[rbp-...] + pops) -> forzar frame (no
                 * frameless), igual que un call normal. */
                if (in.op == MOp::TAILCALL) has_calls = true;
                if (in.op == MOp::ALLOCA) {
                    const uint32_t sz = static_cast<uint32_t>(in.src1.value);
                    alloca_total += (sz + 7u) & ~7u;
                }
                if (in.op == MOp::ALLOCA_VM) has_vm_alloca = true;
            }
        }
        Lowerer lw(ra, tri, abi, has_calls, alloca_total, has_vm_alloca);
        lw.ivs = ivs;  // commit 6: para construir stackmaps en CALLs
        MFunction pf;
        lw.pf = &pf;   // labels intra-expansion (LOAD_VM/STORE_VM page-cache)
        pf.name          = vf.name;
        pf.next_label_id  = vf.next_label_id;
        pf.label_offsets  = vf.label_offsets;
        pf.imm64_pool     = vf.imm64_pool;
        pf.blocks.resize(vf.blocks.size());

        /* OSR 1a: deteccion PRECISA de loop back-edges via DFS (clasico:
         * arista u->v es back-edge sii v esta "gris" = en la pila del DFS).
         * Distingue ciclos reales de saltos a un indice menor que NO son loops
         * (e.g. el merge de un if/else anidado).  @c be_block[u]=1 si el bloque
         * u termina en un BR INCONDICIONAL (succ_b invalido) que es back-edge.
         * Iterativo para no desbordar la pila en funciones con muchos bloques. */
        const size_t NB = vf.blocks.size();
        std::vector<uint8_t> be_block;  // vacio salvo con el gate (cero overhead)
        if (jit_osr_count() && NB > 0) {
            be_block.assign(NB, 0);
            std::vector<uint8_t> color(NB, 0);  // 0=white 1=gray 2=black
            std::vector<std::pair<MBlockId, int>> stk;  // (bloque, prox succ)
            stk.push_back({0, 0});
            color[0] = 1;
            while (!stk.empty()) {
                MBlockId u = stk.back().first;
                int &si = stk.back().second;
                const MBlockId succ[2] = { vf.blocks[u].succ_a, vf.blocks[u].succ_b };
                if (si < 2) {
                    const MBlockId v = succ[si];
                    ++si;
                    if (v == MBLOCK_INVALID || v >= NB) continue;
                    if (color[v] == 0) { color[v] = 1; stk.push_back({v, 0}); }
                    else if (color[v] == 1) {
                        /* back-edge u->v; instrumentable solo si u es BR
                         * incondicional (sin succ_b) y v es ese succ_a. */
                        if (vf.blocks[u].succ_b == MBLOCK_INVALID
                         && vf.blocks[u].succ_a == v)
                            be_block[u] = 1;
                    }
                } else {
                    color[u] = 2;
                    stk.pop_back();
                }
            }
        }

        /* OSR 2a: indice global de la primera instr de cada bloque (MISMA
         * numeracion que build_intervals).  @c block_start[h] = 2*first_gi[h]
         * es la posicion del header; @c covers(block_start[h]) == el live-in
         * del header (los valores que C2 debe reanudar).  Solo bajo el gate. */
        std::vector<uint32_t> first_gi;
        if ((jit_osr_count() || osr != nullptr) && NB > 0) {
            first_gi.assign(NB, 0);
            uint32_t g = 0;
            for (size_t bb = 0; bb < NB; ++bb) {
                first_gi[bb] = g;
                g += static_cast<uint32_t>(vf.blocks[bb].instrs.size());
            }
        }

        /* gi = indice lineal global de la instr vreg (MISMO orden que
         * build_intervals).  cur_call_pos = 2*gi se pasa a lower() para que
         * el CALL_ABS sepa que GC roots estan vivos en ese punto. */
        uint32_t gi = 0;
        for (size_t b = 0; b < vf.blocks.size(); ++b) {
            pf.blocks[b].label_id = vf.blocks[b].label_id;
            pf.blocks[b].succ_a   = vf.blocks[b].succ_a;
            pf.blocks[b].succ_b   = vf.blocks[b].succ_b;
            std::vector<MInstr> outv;
            outv.reserve(vf.blocks[b].instrs.size() * 2 + 8);
            if (b == 0) lw.emit_prologue(outv);
            for (const MInstr &in : vf.blocks[b].instrs) {
                lw.cur_call_pos = 2u * gi;
                lw.lower(in, outv);
                ++gi;
            }
            /* OSR 1a: instrumentar el back-edge (BR incondicional a un bloque
             * anterior).  El counter va ANTES del JMP terminal; el `add` toca
             * flags pero el JMP no las lee.  push/pop rax preserva el estado.
             * Emitido aqui (post-lower) para no pasar por la legalizacion
             * 2-address que mangla ADD [mem],imm. */
            if (osr == nullptr && jit_osr_count() && be_block[b]
             && !outv.empty() && outv.back().op == MOp::JMP) {
                osr_install_dump_once();
                const uint64_t loop_id = g_osr_be_sites;
                ++g_osr_be_sites;

                /* --- OSR 2a: descriptor del estado vivo del loop header ---
                 * Capturamos el live-in del header = covers(2*first_gi[header])
                 * = phi_dst (loop-carried) + invariantes usados en el loop
                 * (excluye phi_args y temporales del header).  Cada valor se
                 * escribira en osr_buffer[vid].  Restringido a IR values reales
                 * (vid < ir_value_count): esos ids son ESTABLES entre C1 y C2
                 * (el clon C2 los preserva), garantizando que C1 escribe y C2
                 * lee la MISMA celda.  Si algun valor no es capturable (temp del
                 * selector, fuera del buffer, o sin ubicacion fisica), ABORTA la
                 * captura de este loop (el trigger sigue logueando "ABORTADO"). */
                const MBlockId header = vf.blocks[b].succ_a;
                OsrLoopDesc desc;
                desc.fn_name      = vf.name;
                desc.header_block = header;
                desc.aborted      = false;
                if (ivs != nullptr && header != MBLOCK_INVALID
                 && static_cast<size_t>(header) < NB && !first_gi.empty()) {
                    const uint32_t header_pos = 2u * first_gi[header];
                    const uint32_t NVI =
                        static_cast<uint32_t>(ivs->intervals.size());
                    const uint32_t ir_n = vf.ir_value_count;
                    for (uint32_t v = 0; v < NVI; ++v) {
                        const LiveInterval &lv = ivs->intervals[v];
                        if (!lv.covers(header_pos)) continue;
                        if (v >= ir_n || v >= VESTA_OSR_BUFFER_N
                         || (!ra.in_reg(v) && !ra.spilled(v))) {
                            desc.aborted = true; desc.captures.clear(); break;
                        }
                        desc.captures.push_back(
                            { v, static_cast<uint8_t>(lv.is_gc() ? 1u : 0u) });
                    }
                } else {
                    desc.aborted = true;
                }
                const bool do_capture = !desc.aborted;
                /* Copia para el codegen (el descriptor se mueve al registro). */
                const std::vector<OsrCaptureSlot> caps = desc.captures;
                if (g_osr_loops.size() <= static_cast<size_t>(loop_id))
                    g_osr_loops.resize(static_cast<size_t>(loop_id) + 1u);
                g_osr_loops[static_cast<size_t>(loop_id)] = std::move(desc);

                /* Indices del pool: contador, loop_id, handler. */
                const uint32_t idx_cnt = static_cast<uint32_t>(pf.imm64_pool.size());
                pf.imm64_pool.push_back(reinterpret_cast<uint64_t>(&g_osr_be_total));
                const uint32_t idx_lid = static_cast<uint32_t>(pf.imm64_pool.size());
                pf.imm64_pool.push_back(loop_id);
                const uint32_t idx_h = static_cast<uint32_t>(pf.imm64_pool.size());
                pf.imm64_pool.push_back(
                    reinterpret_cast<uint64_t>(&osr_trigger_stub));
                const MLabelId lskip = pf.new_label();
                auto R = [](MReg r) { return MOperand::make_reg(r, 8); };
                std::vector<MInstr> seq;
                /* --- contador + check (cada iteracion; jne bien predicho) --- */
                seq.push_back(MInstr::make_unary(MOp::PUSH, {}, R(MReg::RAX)));
                seq.push_back(MInstr::make_unary(MOp::MOV, R(MReg::RAX),
                    MOperand::make_imm64_idx(idx_cnt)));
                seq.push_back(MInstr::make_unary(MOp::ADD,
                    MOperand::make_mem(MReg::RAX, 0), MOperand::make_imm32(1)));
                seq.push_back(MInstr::make_unary(MOp::CMP,
                    MOperand::make_mem(MReg::RAX, 0),
                    MOperand::make_imm32(static_cast<int32_t>(jit_osr_threshold()))));
                seq.push_back(MInstr::make_unary(MOp::POP, R(MReg::RAX), {}));
                seq.push_back(MInstr::make_jcc(MCond::NE, lskip));  // cmp no toca rsp; pop no toca flags
                /* --- trigger one-shot (cnt == umbral): preservar TODO el estado
                 * vivo (caller-saved), llamar al handler, restaurar.  RBX
                 * (=ProcessVM*) es callee-saved -> sobrevive el call. --- */
                const auto &cs = tri.caller_saved[static_cast<size_t>(RegClass::GP)];
                for (uint8_t r : cs)
                    seq.push_back(MInstr::make_unary(MOp::PUSH, {}, R(static_cast<MReg>(r))));
#if defined(_WIN32)
                const MReg A0 = MReg::RCX, A1 = MReg::RDX, A2 = MReg::R8;
                const uint32_t SHADOW = 32;
#else
                const MReg A0 = MReg::RDI, A1 = MReg::RSI, A2 = MReg::RDX;
                const uint32_t SHADOW = 0;
#endif
                /* --- OSR 2a captura: escribir cada vid vivo en osr_buffer[vid].
                 * RAX = base del buffer (proc->osr_buffer leido via RBX); RCX =
                 * temp.  Ambos son caller-saved (ya salvados en la pila arriba)
                 * y se restauran en el pop final.  Las fuentes caller-saved se
                 * leen de su COPIA en la pila (offset = 8*(n-1-idx), nunca del
                 * reg vivo, que podemos haber clobreado); las callee-saved del
                 * reg directo (intactas); las derramadas del slot [rbp+off]. */
                if (do_capture) {
                    const size_t n = cs.size();
                    seq.push_back(MInstr::make_unary(MOp::MOV, R(MReg::RAX),
                        MOperand::make_mem(MReg::RBX,
                            VESTA_PROC_OSR_BUFFER_OFFSET)));
                    for (const OsrCaptureSlot &c : caps) {
                        const MOperand dstmem = MOperand::make_mem(
                            MReg::RAX, static_cast<int32_t>(c.vid * 8u));
                        if (ra.in_reg(c.vid)) {
                            const uint8_t rid = ra.reg_of(c.vid);
                            int idx = -1;
                            for (size_t i = 0; i < n; ++i)
                                if (cs[i] == rid) {
                                    idx = static_cast<int>(i); break;
                                }
                            if (idx >= 0) {
                                /* caller-saved: leer de la copia en la pila. */
                                const int32_t off = static_cast<int32_t>(
                                    8u * (n - 1u - static_cast<size_t>(idx)));
                                seq.push_back(MInstr::make_unary(MOp::MOV,
                                    R(MReg::RCX),
                                    MOperand::make_mem(MReg::RSP, off)));
                                seq.push_back(MInstr::make_unary(MOp::MOV,
                                    dstmem, R(MReg::RCX)));
                            } else {
                                /* callee-saved: valor vivo en el reg directo. */
                                seq.push_back(MInstr::make_unary(MOp::MOV,
                                    dstmem, R(static_cast<MReg>(rid))));
                            }
                        } else {  /* spilled: leer del slot rbp-relativo. */
                            seq.push_back(MInstr::make_unary(MOp::MOV,
                                R(MReg::RCX),
                                MOperand::make_mem(MReg::RBP,
                                    lw.slot_off(ra.slot_of(c.vid)))));
                            seq.push_back(MInstr::make_unary(MOp::MOV,
                                dstmem, R(MReg::RCX)));
                        }
                    }
                    /* arg2 = base del buffer (sigue en RAX tras la captura). */
                    seq.push_back(MInstr::make_unary(MOp::MOV, R(A2),
                        R(MReg::RAX)));
                } else {
                    /* sin captura: arg2 = 0 (el handler no lee el buffer). */
                    seq.push_back(MInstr::make_unary(MOp::MOV, R(A2),
                        MOperand::make_imm32(0)));
                }
                seq.push_back(MInstr::make_unary(MOp::MOV, R(A0), R(MReg::RBX)));  // arg0=proc
                seq.push_back(MInstr::make_unary(MOp::MOV, R(A1),
                    MOperand::make_imm64_idx(idx_lid)));                           // arg1=loop_id
                /* Alinear rsp a 16 + shadow space (Win64).  Tras el frame rsp
                 * esta 16-alineado; push de C caller-saved lo desalinea 8 si C
                 * es impar. */
                const uint32_t adjust = ((cs.size() & 1u) ? 8u : 0u) + SHADOW;
                if (adjust)
                    seq.push_back(MInstr::make_unary(MOp::SUB, R(MReg::RSP),
                        MOperand::make_imm32(static_cast<int32_t>(adjust))));
                seq.push_back(MInstr::make_unary(MOp::MOV, R(MReg::RAX),
                    MOperand::make_imm64_idx(idx_h)));
                { MInstr c; c.op = MOp::CALL; c.src1 = R(MReg::RAX); seq.push_back(c); }
                /* --- 2c FRAME-SWAP: el handler devolvio en RAX la direccion del
                 * OSR-entry del C2 (o 0).  Si != 0, ABANDONAMOS el frame C1 y
                 * saltamos al C2: emit_epilogue restaura el frame con
                 * `lea rsp,[rbp-8*total_saved]` (ABSOLUTO desde rbp -> no hay que
                 * deshacer los push del trigger) + pops, dejando rsp en la
                 * direccion de retorno del CALLER de C1.  RAX (osr_entry) NO se
                 * popea en el epilogue -> sobrevive.  `jmp rax` entra al C2 con
                 * rsp en ret_addr: el RET del C2 retorna al caller de C1, como si
                 * el C2 hubiera sido la funcion llamada.  El buffer-por-VID ya
                 * tiene el estado (capturado arriba, antes del handler) y el
                 * handler es lookup-puro (sin GC) -> host_ptr GC siguen frescos. */
                const MLabelId lcont = pf.new_label();
                seq.push_back(MInstr::make_unary(MOp::TEST, R(MReg::RAX),
                                                 R(MReg::RAX)));
                seq.push_back(MInstr::make_jcc(MCond::E, lcont));  // RAX==0 -> no swap
                /* El OSR-entry del C2 es VM_ABI: su prologue hace
                 * `mov rbx, <arg0>` esperando ProcessVM* en el registro del 1er
                 * argumento (A0 = RCX win / RDI sysv).  El epilogue del C1
                 * restaura RBX al valor del caller (pop rbx) -> hay que pasar
                 * proc por A0 ANTES del epilogue.  A0 es caller-saved y NO esta
                 * en callee_saved -> sobrevive el epilogue (igual que RAX). */
                seq.push_back(MInstr::make_unary(MOp::MOV, R(A0), R(MReg::RBX)));
                lw.emit_epilogue(seq);                            // rsp -> ret_addr; RAX + A0 intactos
                { MInstr j; j.op = MOp::JMP; j.src1 = R(MReg::RAX); seq.push_back(j); }
                seq.push_back(MInstr::make_label_def(lcont));
                /* --- ruta sin swap (RAX==0): continuar el loop C1 normal. --- */
                if (adjust)
                    seq.push_back(MInstr::make_unary(MOp::ADD, R(MReg::RSP),
                        MOperand::make_imm32(static_cast<int32_t>(adjust))));
                for (size_t i = cs.size(); i-- > 0;)
                    seq.push_back(MInstr::make_unary(MOp::POP,
                        R(static_cast<MReg>(cs[i])), {}));
                seq.push_back(MInstr::make_label_def(lskip));
                outv.insert(outv.end() - 1, seq.begin(), seq.end());
            }
            pf.blocks[b].instrs = std::move(outv);
        }

        /* --- OSR 2b: APPEND del bloque OSR-entry (C2, el 2o punto de entrada
         * a mitad del loop).  Monta el frame normal, carga el live-in del
         * header desde proc->osr_buffer[vid] a la ubicacion fisica de cada vid,
         * y salta al MBlock del header.  R10/R11 (scratch, no asignables) como
         * base/temp -> nunca colisionan con la ubicacion fisica de un vid. */
        if (osr != nullptr && ivs != nullptr && !first_gi.empty()
         && osr->header_block != MBLOCK_INVALID
         && static_cast<size_t>(osr->header_block) < NB) {
            const MBlockId header   = osr->header_block;
            const uint32_t header_pos = 2u * first_gi[header];
            const uint32_t NVI = static_cast<uint32_t>(ivs->intervals.size());
            const uint32_t ir_n = vf.ir_value_count;

            /* (0) Recolectar el live-in del header (covers(header_pos)) y, si hay
             * red de seguridad (required_captures), VERIFICAR que todo vid este
             * en el conjunto capturado por el C1.  Si alguno NO esta -> ABORTAR
             * (no emitir el OSR-entry) -> el C2 no se registra -> sin swap -> el
             * C1 continua.  Asi un C2 optimizado que necesite un valor no
             * capturado por el C1 degrada a "sin speedup", nunca a corrupcion. */
            std::vector<uint32_t> live_in;
            bool mismatch = false;
            for (uint32_t v = 0; v < NVI; ++v) {
                const LiveInterval &lv = ivs->intervals[v];
                if (!lv.covers(header_pos)) continue;
                if (v >= ir_n || v >= VESTA_OSR_BUFFER_N) continue;
                if (!ra.in_reg(v) && !ra.spilled(v)) continue;  // sin ubicacion
                if (osr->required_captures != nullptr) {
                    const auto &req = *osr->required_captures;
                    bool found = false;
                    for (uint32_t cv : req) if (cv == v) { found = true; break; }
                    if (!found) { mismatch = true; break; }
                }
                live_in.push_back(v);
            }
            if (mismatch) {
                osr->osr_entry_valid = false;  // fallback seguro: no swap
            } else {
                const MLabelId lbl = pf.new_label();
                auto R = [](MReg r) { return MOperand::make_reg(r, 8); };
                const MReg base = lw.scr0;  // R10 (no asignable)
                const MReg tmp  = lw.scr1;  // R11 (no asignable)
                MBlock entryb;
                entryb.label_id = lbl;
                entryb.succ_a   = header;       // metadata (el jmp usa el label)
                entryb.succ_b   = MBLOCK_INVALID;
                std::vector<MInstr> ob;
                /* (a) Prologue IDENTICO al de la entry 0 (mismo frame). */
                lw.emit_prologue(ob);
                /* (b) base = proc->osr_buffer (via RBX = ProcessVM*). */
                ob.push_back(MInstr::make_unary(MOp::MOV, R(base),
                    MOperand::make_mem(MReg::RBX, VESTA_PROC_OSR_BUFFER_OFFSET)));
                /* (c) cargar cada live-in vid del buffer a su reg/slot. */
                for (uint32_t v : live_in) {
                    const MOperand srcmem = MOperand::make_mem(
                        base, static_cast<int32_t>(v * 8u));
                    if (ra.in_reg(v)) {
                        ob.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(static_cast<MReg>(ra.reg_of(v)), 8),
                            srcmem));
                    } else {  /* spilled (garantizado por el filtro de arriba). */
                        ob.push_back(MInstr::make_unary(MOp::MOV, R(tmp), srcmem));
                        ob.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_mem(MReg::RBP,
                                lw.slot_off(ra.slot_of(v))), R(tmp)));
                    }
                }
                /* (d) saltar al header (reanuda el loop a mitad). */
                ob.push_back(MInstr::make_jmp(pf.blocks[header].label_id));
                entryb.instrs = std::move(ob);
                pf.blocks.push_back(std::move(entryb));
                osr->osr_entry_label = lbl;
                osr->osr_entry_valid = true;
            }
        }

        /* --- Peephole final: eliminar `mov r64, r64` con el MISMO registro
         * (no-ops puros del two-address legalization: `mov pdst, rs1` cuando
         * pdst==rs1).  En un loop apretado (p.ej. un C2 con inline agresivo)
         * estos consumen ancho de banda de DECODE aunque el renamer los elimine.
         * SOLO width 8: un `mov e_x, e_x` de 32 bits zero-extiende los 64 bits
         * (NO es no-op) y se conserva.  Cero riesgo: un self-mov de 64 bits es
         * semanticamente identico a su ausencia. */
        for (auto &blk : pf.blocks) {
            std::vector<MInstr> cleaned;
            cleaned.reserve(blk.instrs.size());
            for (MInstr &mi : blk.instrs) {
                if (mi.op == MOp::MOV
                 && mi.dst.kind == MOperandKind::REG
                 && mi.src1.kind == MOperandKind::REG
                 && mi.dst.reg == mi.src1.reg
                 && mi.dst.width == 8 && mi.src1.width == 8) {
                    continue;  // self-mov de 64 bits -> descartar
                }
                cleaned.push_back(std::move(mi));
            }
            blk.instrs = std::move(cleaned);
        }

        pf.stackmaps = std::move(lw.stackmaps);  // commit 6
        return pf;
    }

    /* ===================================================================== */
    /* OSR runtime glue (Phase D.8, 2c) -- definiciones publicas              */
    /* ===================================================================== */

    void set_osr_handler(uint64_t (*handler)(uint64_t)) {
        g_osr_handler = handler;
    }

    uint32_t osr_loop_count() {
        return g_osr_be_sites;
    }

    bool osr_loop_info(uint64_t loop_id, std::string &fn_name_out,
                       uint32_t &header_block_out) {
        if (loop_id >= g_osr_loops.size()) return false;
        const OsrLoopDesc &d = g_osr_loops[static_cast<size_t>(loop_id)];
        if (d.aborted) return false;
        fn_name_out      = d.fn_name;
        header_block_out = d.header_block;
        return true;
    }

    bool osr_loop_captures(uint64_t loop_id, std::vector<uint32_t> &out_vids) {
        if (loop_id >= g_osr_loops.size()) return false;
        const OsrLoopDesc &d = g_osr_loops[static_cast<size_t>(loop_id)];
        if (d.aborted) return false;
        out_vids.clear();
        out_vids.reserve(d.captures.size());
        for (const OsrCaptureSlot &c : d.captures) out_vids.push_back(c.vid);
        return true;
    }

} // namespace jit
