/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/selector.cpp
 * @brief Implementacion del instruction selector @c IrFunction -> @c MFunction.
 *
 * = Algoritmo =
 *
 * Para cada IrInstr emite una plantilla fija:
 *
 *   1. LOAD operandos a regs scratch (rax, rcx, rdx, r10, r11).
 *   2. Compute (op apropiada en MachineIR).
 *   3. STORE resultado al slot del dst (si dst != IR_NO_VALUE).
 *
 * Cada SSA value vid tiene slot stack en offset @c -8*(vid+1) desde
 * RBP.  Frame total = @c 8*num_values bytes alineado a 16.
 *
 * Para terminadores:
 *   - RET %v: LOAD %v -> rax; LEAVE; RET
 *   - BR target: emite label si necesario y JMP target
 *   - BR_COND %cond, true_bb, false_bb: LOAD %cond; CMP rax, 0;
 *     JNE label(true_bb); JMP label(false_bb)
 *
 * El selector mantiene un @c std::vector<MLabelId> @c block_labels_
 * que mapea ir_block_id -> MLabelId, asi BR/BR_COND saltan a
 * etiquetas estables.
 */

#include "jit/selector.h"

#include "jit/auto_jit.h"
#include "jit/jit_regalloc.h"
#include "vesta_rt/abi.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace jit {

    namespace {

        /* ----- Helpers para el mini-parser de raw_asm (Phase D.3-G) ----- */

        /** @brief Slot index del VM register en proc->registers (0..15 = r0..r15,
         *         16 = rsp, 17 = rbp).  -1 si no reconoce. */
        inline int vm_reg_slot_index(const std::string &name) noexcept {
            if (name == "rsp") return 16;
            if (name == "rbp") return 17;
            if (name.size() >= 2 && name[0] == 'r') {
                int n = 0;
                size_t i = 1;
                while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
                    n = n * 10 + (name[i] - '0');
                    ++i;
                }
                if (i == name.size() && n >= 0 && n <= 15) return n;
            }
            return -1;
        }

        /** @brief Offset en proc para el slot del VM reg.
         *
         * Layout (verificado en abi_checks.cpp):
         *   - regs[0..15] empiezan en VESTA_PROC_REGISTERS_OFFSET.
         *   - stack_pointer y base_pointer viven en context_registers_vm
         *     ANTES de regs[].  context_registers_vm tiene:
         *       offset 0:  stack_pointer (8B)
         *       offset 8:  base_pointer  (8B)
         *       offset 16: rip           (8B)
         *       offset 24: flags         (8B)
         *       offset 32: regs[0]       (16 * 8 = 128B)
         *   Por tanto offsetof(ProcessVM, registers) = VESTA_PROC_REGISTERS_OFFSET - 32.
         *   Slot 16 ("rsp") -> registers + 0  = VESTA_PROC_REGISTERS_OFFSET - 32.
         *   Slot 17 ("rbp") -> registers + 8  = VESTA_PROC_REGISTERS_OFFSET - 24. */
        inline int32_t vm_reg_offset(int slot) noexcept {
            if (slot == 16) return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) - 32;
            if (slot == 17) return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) - 24;
            return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) + slot * 8;
        }

        inline std::string trim_str(const std::string &s) {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
            return s.substr(a, b - a);
        }

        inline std::vector<std::string> split_csv(const std::string &s) {
            std::vector<std::string> out;
            std::string cur;
            int depth = 0;
            bool quote = false;
            for (char c : s) {
                if (quote) { cur.push_back(c); if (c == '"') quote = false; continue; }
                if (c == '"') { quote = true; cur.push_back(c); continue; }
                if (c == '(') { ++depth; cur.push_back(c); continue; }
                if (c == ')') { --depth; cur.push_back(c); continue; }
                if (c == ',' && depth == 0) { out.push_back(trim_str(cur)); cur.clear(); continue; }
                cur.push_back(c);
            }
            if (!cur.empty()) out.push_back(trim_str(cur));
            return out;
        }

        inline bool parse_imm_int(const std::string &s, int64_t &out) {
            if (s.empty()) return false;
            try {
                size_t pos = 0;
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    out = static_cast<int64_t>(std::stoull(s.substr(2), &pos, 16));
                    return pos == s.size() - 2;
                }
                if (s[0] == '-' || std::isdigit(static_cast<unsigned char>(s[0]))) {
                    out = std::stoll(s, &pos, 10);
                    return pos == s.size();
                }
                return false;
            } catch (...) {
                return false;
            }
        }

        /** @brief Si `[rN]`, retorna nombre del reg interior; sino vacio. */
        inline std::string parse_mem_operand(const std::string &s) {
            if (s.size() < 3 || s.front() != '[' || s.back() != ']') return {};
            return trim_str(s.substr(1, s.size() - 2));
        }

        /* Regs scratch: usamos solo caller-saved que ningun ABI preserva
         * para evitar push/pop adicionales en el prologue/epilogue. */
        constexpr MReg SCRATCH_A = MReg::RAX;
        constexpr MReg SCRATCH_B = MReg::RCX;
        constexpr MReg SCRATCH_C = MReg::RDX;

        /** @brief Offset del slot stack de un SSA value (RBP - offset). */
        inline int32_t slot_offset(ir::IrValueId vid) noexcept {
            return -8 * (static_cast<int32_t>(vid) + 1);
        }

        /** @brief MOperand de un slot stack (RBP - offset). */
        inline MOperand slot_mem(ir::IrValueId vid) noexcept {
            return MOperand::make_mem(MReg::RBP, slot_offset(vid));
        }

        /** @brief Emite MOV reg, [slot] (LOAD operand). */
        void load_op(MFunction &mf, ir::IrValueId vid, MReg dst) {
            mf.blocks.back().instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_reg(dst),
                    slot_mem(vid)));
        }

        /** @brief Emite MOV [slot], reg (STORE result). */
        void store_op(MFunction &mf, ir::IrValueId vid, MReg src) {
            if (vid == ir::IR_NO_VALUE) return;
            mf.blocks.back().instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    slot_mem(vid),
                    MOperand::make_reg(src)));
        }

        /**
         * @brief resuelve un nombre de funcion runtime a su
         *        direccion via @c RuntimeEntries.
         *
         * Los IR @c IrOp::CALL pueden referenciar:
         *   - Funciones del lenguaje del usuario (sin soporte v1).
         *   - Wrappers runtime @c vrt_* (esta tabla).
         *   - CALLN nativo via lib:fn (sin soporte v1).
         *
         * Para v1 reconocemos solo los nombres @c vrt_* canonicos y
         * devolvemos el field correspondiente de @c RuntimeEntries.
         * Retorna 0 si el nombre no es reconocido.
         */
        inline uint64_t resolve_runtime_entry(const std::string &name,
                                              const RuntimeEntries *rt) {
            if (!rt) return 0;
            #define MATCH(n, field) \
                if (name == #n) return reinterpret_cast<uint64_t>(rt->field)
            MATCH(vrt_gc_alloc,         gc_alloc);
            MATCH(vrt_gc_alloc_pinned,  gc_alloc_pinned);
            MATCH(vrt_gc_deref,         gc_deref);
            MATCH(vrt_gc_handle_for_ptr,gc_handle_for_ptr);
            MATCH(vrt_gc_drop,          gc_drop);
            MATCH(vrt_gc_addref,        gc_addref);
            MATCH(vrt_gc_release,       gc_release);
            MATCH(vrt_gc_write_barrier, gc_write_barrier);
            MATCH(vrt_monitor_enter,    monitor_enter);
            MATCH(vrt_monitor_exit,     monitor_exit);
            MATCH(vrt_monitor_wait,     monitor_wait);
            MATCH(vrt_monitor_notify,   monitor_notify);
            MATCH(vrt_monitor_notify_all, monitor_notify_all);
            MATCH(vrt_throw_fatal,      throw_fatal);
            MATCH(vrt_tryenter,         tryenter);
            MATCH(vrt_tryleave,         tryleave);
            MATCH(vrt_invoke_native,    invoke_native);
            MATCH(vrt_safepoint_poll,   safepoint_poll);
            MATCH(vrt_safepoint_handler,safepoint_handler);
            #undef MATCH
            return 0;
        }

        /** @brief Tamano en bytes de un @c IrType (mismo que ir_emitter). */
        inline uint64_t ir_type_size_bytes(ir::IrType t) noexcept {
            switch (t) {
                case ir::IrType::I8:  case ir::IrType::U8:  case ir::IrType::BOOL: return 1;
                case ir::IrType::I16: case ir::IrType::U16: return 2;
                case ir::IrType::I32: case ir::IrType::U32: case ir::IrType::F32: return 4;
                default: return 8;
            }
        }

        /** @brief True si el tipo entero es con signo (I8/I16/I32/I64). */
        inline bool ir_type_is_signed_int(ir::IrType t) noexcept {
            return t == ir::IrType::I8  || t == ir::IrType::I16
                || t == ir::IrType::I32 || t == ir::IrType::I64;
        }

        /** @brief Cond code x86 correspondiente al IrOp signed/unsigned. */
        inline MCond cond_for_cmp_op(ir::IrOp op) {
            switch (op) {
                case ir::IrOp::CMP_EQ:  return MCond::E;
                case ir::IrOp::CMP_NE:  return MCond::NE;
                case ir::IrOp::CMP_LT:  return MCond::L;
                case ir::IrOp::CMP_GT:  return MCond::G;
                case ir::IrOp::CMP_LE:  return MCond::LE;
                case ir::IrOp::CMP_GE:  return MCond::GE;
                case ir::IrOp::CMP_ULT: return MCond::B;
                case ir::IrOp::CMP_UGT: return MCond::A;
                case ir::IrOp::CMP_ULE: return MCond::BE;
                case ir::IrOp::CMP_UGE: return MCond::AE;
                default:                return MCond::NONE;
            }
        }

    } // namespace anonymous

    /* ===================================================================== */
    /* select                                                                 */
    /* ===================================================================== */

    MFunction Selector::select(const ir::IrFunction &ir_fn, bool *out_unsupported) {
        MFunction mf;
        mf.name = ir_fn.name;
        bool unsupported = false;

        /* regalloc del JIT.
         *
         * Computa una asignacion estatica de los VIDs mas usados a
         * registros callee-saved (R12-R15).  El selector emite el
         * codigo IGUAL que antes (siempre via slot stack); tras la
         * generacion, aplicamos @c apply_jit_regalloc_rewrite que
         * recorre las MInstrs y reemplaza accesos a @c [rbp+slot]
         * con accesos directos al reg asignado.
         *
         * Para que el alignment del stack se mantenga, el regalloc
         * fuerza un numero PAR de regs usados (0, 2, o 4).  El
         * prologue/epilogue pushea/pop-ea cada uno antes/despues del
         * @c push rbp.
         *
         * Si @c regalloc.empty(), nada cambia respecto al path original. */
        const JitRegalloc regalloc = compute_jit_regalloc(ir_fn);

        /* Set para deduplicar warnings de IR ops no soportadas dentro de
         * la misma funcion: la misma op repetida en multiples lineas o
         * el mismo (op, linea) en multiples puntos solo se reporta una vez.
         * Key: (op_enum_value, source_line).  Solo activo si
         * g_jit_warn_unsupported esta seteado. */
        std::set<std::pair<int, uint32_t>> warned_ops;
        auto warn_unsupported = [&](ir::IrOp op, uint32_t line, const char *detail) {
            if (!jit::g_jit_warn_unsupported) return;
            auto key = std::make_pair(static_cast<int>(op), line);
            if (!warned_ops.insert(key).second) return;
            if (detail && detail[0]) {
                std::fprintf(stderr,
                    "[jit] selector: op %s no soportada (%s) en fn '%s' linea %u\n",
                    ir::ir_op_name(op), detail, ir_fn.name.c_str(), line);
            } else {
                std::fprintf(stderr,
                    "[jit] selector: op %s no soportada en fn '%s' linea %u\n",
                    ir::ir_op_name(op), ir_fn.name.c_str(), line);
            }
        };

        /* Phase: frame size con shadow space + alignment correcto.
         *
         * Win64 ABI:
         *   - Stack debe estar 16-byte aligned IMMEDIATELY before CALL.
         *   - Caller debe reservar 32 bytes de SHADOW SPACE (home space)
         *     que la callee usa para spill de RCX/RDX/R8/R9.
         *
         * Layout del stack en function entry:
         *   entry_rsp = caller_16k - 8  (CALL pusheo retaddr)
         *
         * Tras prologue (push rbx + push rbp + sub rsp, FRAME):
         *   rsp = entry_rsp - 16 - FRAME = caller_16k - 24 - FRAME
         *
         * Para que rsp este 16-aligned antes de NUESTROS calls:
         *   (caller_16k - 24 - FRAME) mod 16 == 0
         *   -24 - FRAME ≡ 0 (mod 16)
         *   FRAME ≡ -24 ≡ 8 (mod 16)
         *
         * Asi FRAME debe ser de la forma 8 + 16k.
         *
         * Plus 32 bytes shadow space para Win64.
         *
         * Total: FRAME = (slot_bytes alineado a 16) + 8 + 32 = slot_bytes + 40.
         * Resultado: multiplo de 16 + 8 = de la forma 8 + 16k. Win64 OK.
         *
         * En SysV (Linux/macOS): no hay shadow, solo el alignment.
         *   FRAME = slot_bytes + 8 (de la forma 8 + 16k).
         *
         * IMPORTANTE: los slot offsets siguen siendo [rbp-8*(vid+1)].
         * El ALIGN_PAD + SHADOW estan al BOTTOM del frame (en rsp side),
         * no interfieren con slots (que cuentan desde rbp hacia abajo).
         */
        const size_t num_values = ir_fn.values.size();
        uint32_t slot_bytes = static_cast<uint32_t>(num_values * 8);
        if (slot_bytes & 15) slot_bytes = (slot_bytes + 15) & ~15u;
        /* Padding fijo para alinear rsp + shadow space (solo Win64). */
        constexpr uint32_t ALIGN_PAD = 8;
#if defined(_WIN32)
        constexpr uint32_t SHADOW_SPACE = 32;
#else
        constexpr uint32_t SHADOW_SPACE = 0;
#endif
        uint32_t frame_size = slot_bytes + ALIGN_PAD + SHADOW_SPACE;
        mf.stack_frame_size = frame_size;

        /* Internar la direccion del safepoint handler en el imm64_pool
         * si VM_ABI esta activo y tenemos un handler valido. */
        if (opts_.mode == SelectorMode::VM_ABI
         && opts_.safepoint_handler_addr != 0) {
            safepoint_pool_idx_ = mf.intern_imm64(opts_.safepoint_handler_addr);
        } else {
            safepoint_pool_idx_ = UINT32_MAX;
        }

        /* Fase 1: tracker de GC slots vivos.
         *
         * Para correctness, la regla es:
         *   - Un slot se marca como GC cuando se ESCRIBE un SSA value
         *     con @c is_gc_object = true.
         *   - Una vez marcado, permanece marcado hasta el RET (over-
         *     conservative pero seguro - C1 template no reutiliza slots).
         *
         * En cada SAFEPOINT/CALL emitido, hacemos snapshot del set actual
         * y lo guardamos como Stackmap en mf.stackmaps.  El indice del
         * stackmap se almacena en @c MInstr::flags para que el encoder
         * pueda relacionarlo durante emit. */
        std::vector<StackmapSlot> live_gc_slots;
        auto emit_stackmap_for_safepoint = [&](MInstr &sp_instr) {
            /* Crear stackmap con copia del set actual. */
            Stackmap sm;
            sm.pc_offset = 0;  /* lo rellena el encoder */
            sm.slots = live_gc_slots;
            mf.stackmaps.push_back(std::move(sm));
            const size_t idx = mf.stackmaps.size() - 1;
            /* Limitar a uint16_t para que quepa en MInstr::flags. */
            if (idx <= UINT16_MAX) {
                sp_instr.flags = static_cast<uint16_t>(idx);
            } else {
                /* Demasiados stackmaps; saturamos al ultimo valido.
                 * En la practica > 65535 safepoints por funcion es absurdo. */
                sp_instr.flags = UINT16_MAX;
            }
        };
        auto mark_slot_as_gc_if_needed = [&](ir::IrValueId vid, ir::IrType /*type*/) {
            if (vid == ir::IR_NO_VALUE) return;
            if (vid >= ir_fn.values.size()) return;
            if (!ir_fn.values[vid].is_gc_object) return;
            /* Anyadir slot al set si no esta ya. */
            const int16_t off = static_cast<int16_t>(slot_offset(vid));
            for (const auto &s : live_gc_slots) {
                if (s.rbp_offset == off) return;  /* ya marcado */
            }
            StackmapSlot s;
            s.rbp_offset = off;
            /* Por defecto HANDLE; si es is_host_ptr y type=PTR -> HOSTPTR. */
            s.gc_kind = ir_fn.values[vid].is_host_ptr
                        ? StackmapGcKind::HOSTPTR
                        : StackmapGcKind::HANDLE;
            live_gc_slots.push_back(s);
        };

        /* Reservar labels para cada IrBlock. */
        std::vector<MLabelId> block_labels(ir_fn.blocks.size(), MLABEL_INVALID);
        for (size_t i = 0; i < ir_fn.blocks.size(); ++i) {
            block_labels[i] = mf.new_label();
        }

        /* Prologue: emitir en un bloque inicial.
         *
         * NATIVE_ABI:
         *     push rbp ; mov rbp, rsp ; sub rsp, frame_size
         *
         * VM_ABI:
         *     push rbx                  ; preservar callee-saved
         *     mov rbx, rdi/rcx          ; ProcessVM* va a RBX para safepoints
         *     push rbp ; mov rbp, rsp
         *     sub rsp, frame_size
         */
        MBlockId prologue = mf.new_block(mf.new_label());

        if (opts_.mode == SelectorMode::VM_ABI) {
            /* push rbx (preservar) */
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RBX)));
            /* mov rbx, <proc_reg> */
#if defined(_WIN32)
            const MReg PROC_REG = MReg::RCX;
#else
            const MReg PROC_REG = MReg::RDI;
#endif
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_reg(MReg::RBX),
                    MOperand::make_reg(PROC_REG)));
        }

        /* preservar regs callee-saved usados por regalloc.
         * Conteo PAR garantizado por @c compute_jit_regalloc para que el
         * alignment del frame no cambie (cada push es 8 bytes; 2 o 4
         * pushes anyaden 16 o 32 bytes, ambos multiplos de 16). */
        for (MReg cs_reg : regalloc.callee_saved_used) {
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(cs_reg)));
        }

        /* push rbp ; mov rbp, rsp */
        mf.blocks[prologue].instrs.push_back(
            MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RBP)));
        mf.blocks[prologue].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RBP),
                MOperand::make_reg(MReg::RSP)));
        if (frame_size > 0) {
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::SUB,
                    MOperand::make_reg(MReg::RSP),
                    MOperand::make_imm32(static_cast<int32_t>(frame_size))));
        }

        /* Params: copiar a slots stack segun el ABI activo. */
        if (opts_.mode == SelectorMode::NATIVE_ABI) {
#if defined(_WIN32)
            const MReg arg_regs[] = {MReg::RCX, MReg::RDX, MReg::R8, MReg::R9};
            const size_t MAX_REG_ARGS = 4;
#else
            const MReg arg_regs[] = {MReg::RDI, MReg::RSI, MReg::RDX,
                                      MReg::RCX, MReg::R8, MReg::R9};
            const size_t MAX_REG_ARGS = 6;
#endif
            for (size_t i = 0; i < ir_fn.params.size() && i < MAX_REG_ARGS; ++i) {
                store_op(mf, ir_fn.params[i], arg_regs[i]);
            }
        } else {
            /* VM_ABI: los args bytecode viven en proc->registers.regs[1..N].
             * Cargar cada uno desde @c [rbx + REGISTERS_OFFSET + i*8] al
             * slot del param IR.
             *
             * Convencion bytecode: R0 es return value (no se inicializa);
             * los args van en R1..R12 (max 12 args).  R15 es argc. */
            for (size_t i = 0; i < ir_fn.params.size() && i < 12; ++i) {
                const int32_t reg_offset =
                    VESTA_PROC_REGISTERS_OFFSET
                    + static_cast<int32_t>((i + 1) * VESTA_REGISTER_SIZE);
                mf.blocks[prologue].instrs.push_back(
                    MInstr::make_unary(MOp::MOV,
                        MOperand::make_reg(SCRATCH_A),
                        MOperand::make_mem(MReg::RBX, reg_offset)));
                store_op(mf, ir_fn.params[i], SCRATCH_A);
                /* Marcar como GC si el param es is_gc_object (e.g. tipo CLASS). */
                mark_slot_as_gc_if_needed(ir_fn.params[i], ir::IrType::VOID);
            }
        }
        /* En NATIVE_ABI tambien marcamos params GC. */
        if (opts_.mode == SelectorMode::NATIVE_ABI) {
            for (auto p : ir_fn.params) {
                mark_slot_as_gc_if_needed(p, ir::IrType::VOID);
            }
        }

        /* Jump al primer IR block (entry). */
        if (!ir_fn.blocks.empty()) {
            mf.blocks[prologue].instrs.push_back(MInstr::make_jmp(block_labels[0]));
        }

        /* Lower cada IR block. */
        for (size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
            const auto &ir_block = ir_fn.blocks[bi];
            MBlockId mb = mf.new_block(block_labels[bi]);

            for (const auto &ins : ir_block.instrs) {
                using IrOp = ir::IrOp;
                switch (ins.op) {
                    /* --------- Mov / Const --------- */
                    case IrOp::NOP: break;
                    case IrOp::PHI: {
                        /* D.3-A: PHI es no-op en el successor.  Los copies
                         * para asignar el valor correcto al slot del dst
                         * ya fueron emitidos en cada predecesor antes de
                         * su BR/BR_COND. */
                        break;
                    }
                    case IrOp::MOV: {
                        if (ins.operands.empty()) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::STR_LIT_ADDR: {
                        /* %dst = direccion VM del literal de string indexado
                         * por @c ins.imm en el bloque "code.s_<imm>" del
                         * .velb.  Equivalente al codigo emitido por el
                         * frontend: `mov rDst, @Absolute("code.s_<imm>")`.
                         * Resolvemos en compile-time via
                         * @c opts_.resolve_symbol que el Loader provee. */
                        const std::string sym = "code.s_" + std::to_string(ins.imm);
                        uint64_t addr = 0;
                        if (opts_.resolve_symbol) {
                            addr = opts_.resolve_symbol(sym);
                        }
                        if (addr == 0) {
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: STR_LIT_ADDR sin symbol '%s' en fn '%s' linea %u\n",
                                        sym.c_str(), ir_fn.name.c_str(), ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* mov rax, addr (via imm64 pool si necesario). */
                        const uint32_t pool_idx = mf.intern_imm64(addr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_imm64_idx(pool_idx)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::GETPROC: {
                        /* %dst = ProcessVM* del proceso actual.  En VM_ABI,
                         * proc esta en RBX (preservado a traves del prologue).
                         * En NATIVE_ABI no hay un proc accesible -- marcar
                         * unsupported. */
                        if (opts_.mode != SelectorMode::VM_ABI) {
                            if (jit::g_jit_warn_unsupported) {
                                std::fprintf(stderr,
                                    "[jit] selector: GETPROC requiere VM_ABI en fn '%s' linea %u\n",
                                    ir_fn.name.c_str(), ins.source_line);
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* mov rax, rbx; store rax -> slot[dst]. */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(MReg::RBX)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::CONST: {
                        if (ins.imm <= 0x7FFFFFFFULL ||
                            static_cast<int64_t>(ins.imm) >= -0x80000000LL) {
                            /* Cabe en imm32 sign-ext.  Usar MOV r, imm32. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_imm32(
                                        static_cast<int32_t>(ins.imm))));
                        } else {
                            /* imm64 via pool. */
                            const uint32_t pool_idx = mf.intern_imm64(ins.imm);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_imm64_idx(pool_idx)));
                        }
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- ALU binarias --------- */
                    case IrOp::ADD:
                    case IrOp::SUB:
                    case IrOp::AND:
                    case IrOp::OR:
                    case IrOp::XOR: {
                        if (ins.operands.size() < 2) break;
                        MOp m_op = MOp::ADD;
                        switch (ins.op) {
                            case IrOp::ADD: m_op = MOp::ADD; break;
                            case IrOp::SUB: m_op = MOp::SUB; break;
                            case IrOp::AND: m_op = MOp::AND; break;
                            case IrOp::OR:  m_op = MOp::OR;  break;
                            case IrOp::XOR: m_op = MOp::XOR; break;
                            default: break;
                        }
                        /* Fold de operando constante: si op1 es CONST y
                         * cabe en imm32, emitir @c ALU scratch, imm32
                         * directamente (evita el slot del const).  Para
                         * ADD/SUB/AND/OR/XOR el encoder ya soporta la
                         * variante imm32. */
                        const ir::IrValueId op0 = ins.operands[0];
                        const ir::IrValueId op1 = ins.operands[1];
                        const bool op1_const = op1 < ir_fn.values.size()
                            && ir_fn.values[op1].is_const;
                        bool used_imm_fold = false;
                        if (op1_const) {
                            const int64_t cv = static_cast<int64_t>(
                                ir_fn.values[op1].const_val);
                            if (cv >= -0x80000000LL && cv <= 0x7FFFFFFFLL) {
                                load_op(mf, op0, SCRATCH_A);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(m_op,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(static_cast<int32_t>(cv))));
                                store_op(mf, ins.dst, SCRATCH_A);
                                used_imm_fold = true;
                            }
                        }
                        /* Op0 const + commutativa: tambien tratamos
                         * @c c + x = x + c.  No para SUB (no conmutativa). */
                        if (!used_imm_fold
                         && (ins.op == IrOp::ADD || ins.op == IrOp::AND
                          || ins.op == IrOp::OR  || ins.op == IrOp::XOR)) {
                            const bool op0_const = op0 < ir_fn.values.size()
                                && ir_fn.values[op0].is_const;
                            if (op0_const) {
                                const int64_t cv = static_cast<int64_t>(
                                    ir_fn.values[op0].const_val);
                                if (cv >= -0x80000000LL && cv <= 0x7FFFFFFFLL) {
                                    load_op(mf, op1, SCRATCH_A);
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(m_op,
                                            MOperand::make_reg(SCRATCH_A),
                                            MOperand::make_imm32(static_cast<int32_t>(cv))));
                                    store_op(mf, ins.dst, SCRATCH_A);
                                    used_imm_fold = true;
                                }
                            }
                        }
                        if (!used_imm_fold) {
                            load_op(mf, op0, SCRATCH_A);
                            load_op(mf, op1, SCRATCH_B);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(m_op,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_reg(SCRATCH_B)));
                            store_op(mf, ins.dst, SCRATCH_A);
                        }
                        break;
                    }
                    case IrOp::MUL: {
                        if (ins.operands.size() < 2) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        load_op(mf, ins.operands[1], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::IMUL,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_B)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- Unarias --------- */
                    case IrOp::NEG: {
                        if (ins.operands.empty()) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::NEG,
                                MOperand::make_reg(SCRATCH_A), {}));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::NOT: {
                        if (ins.operands.empty()) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::NOT,
                                MOperand::make_reg(SCRATCH_A), {}));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- Shifts (imm o reg) --------- */
                    case IrOp::SHL:
                    case IrOp::SHR:
                    case IrOp::SAR: {
                        if (ins.operands.size() < 2) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        /* Detectar si el operando 1 es un CONST en el IR.
                         * Si es, usamos la variante imm8 del shift.  Si
                         * no, fallback a unsupported (el encoder no
                         * soporta shifts via CL aun). */
                        const ir::IrValueId shift_v = ins.operands[1];
                        bool is_const = false;
                        int32_t shift_amt = 0;
                        if (shift_v < ir_fn.values.size()
                         && ir_fn.values[shift_v].is_const) {
                            is_const = true;
                            shift_amt = static_cast<int32_t>(
                                ir_fn.values[shift_v].const_val & 0x3F);
                        }
                        if (!is_const) {
                            warn_unsupported(ins.op, ins.source_line,
                                "shift por reg (solo imm const soportado)");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        MOp mop;
                        switch (ins.op) {
                            case IrOp::SHL: mop = MOp::SHL; break;
                            case IrOp::SHR: mop = MOp::SHR; break;
                            case IrOp::SAR: mop = MOp::SAR; break;
                            default: mop = MOp::SHL; break;
                        }
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(mop,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_imm32(shift_amt)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- DIV / MOD (signed via IDIV) --------- */
                    case IrOp::DIV:
                    case IrOp::MOD: {
                        if (ins.operands.size() < 2) break;
                        /* x86 IDIV usa RDX:RAX dividendo / src -> RAX cociente, RDX resto.
                         * Setup:
                         *   load_op(arg0, RAX)
                         *   load_op(arg1, RCX)
                         *   CQO (sign-extend RAX -> RDX:RAX)
                         *   IDIV RCX
                         *   store_op(dst, RAX si DIV o RDX si MOD)
                         *
                         * RAX y RDX se usan como dividendo (modificados),
                         * RCX se usa como divisor (modificado por la fn
                         * si era operando aliasado).  Nuestro patron de
                         * "load operandos a scratch antes de op" garantiza
                         * que no hay aliasing problematic. */
                        load_op(mf, ins.operands[0], MReg::RAX);
                        load_op(mf, ins.operands[1], MReg::RCX);
                        /* CQO */
                        MInstr cqo;
                        cqo.op = MOp::CQO;
                        mf.blocks.back().instrs.push_back(cqo);
                        /* IDIV RCX */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::IDIV, {},
                                MOperand::make_reg(MReg::RCX)));
                        /* Resultado en RAX (DIV) o RDX (MOD). */
                        if (ins.op == IrOp::DIV) {
                            store_op(mf, ins.dst, MReg::RAX);
                        } else {
                            store_op(mf, ins.dst, MReg::RDX);
                        }
                        break;
                    }

                    /* --------- ALLOCA --------- */
                    case IrOp::ALLOCA: {
                        /* Reservar @c ins.imm bytes en el stack nativo y
                         * almacenar el puntero en el slot del dst.  Estilo
                         * "stack grow" en sitio -- el epilogue (`mov rsp,
                         * rbp; pop rbp`) los libera automaticamente al RET.
                         *
                         * Alineamos a 16 bytes para mantener stack
                         * discipline (calling convention SysV/Win64
                         * requiere rsp 16-aligned ANTES de un CALL). */
                        const uint64_t size_bytes = ins.imm;
                        if (size_bytes > 0 && size_bytes < INT32_MAX) {
                            const uint64_t aligned =
                                (size_bytes + 15ULL) & ~15ULL;
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::SUB,
                                    MOperand::make_reg(MReg::RSP),
                                    MOperand::make_imm32(
                                        static_cast<int32_t>(aligned))));
                        }
                        /* Guardar el puntero (rsp actual) en el slot dst. */
                        store_op(mf, ins.dst, MReg::RSP);
                        break;
                    }

                    /* --------- Conversiones de tipo enteras --------- */
                    /* SEXT/ZEXT/TRUNC/CAST/BITCAST entre tipos enteros.
                     * Algoritmo (mismo que el interp en ir_emitter.cpp):
                     *   - dst_bytes < src_bytes (TRUNC):
                     *       dst_signed: SHL rd, 64-dst_bits; SAR rd, 64-dst_bits
                     *       !dst_signed: SHL rd, 64-dst_bits; SHR rd, 64-dst_bits
                     *   - dst_bytes > src_bytes (ZEXT/SEXT widening):
                     *       src_signed: SHL rd, 64-src_bits; SAR rd, 64-src_bits
                     *       !src_signed: SHL rd, 64-src_bits; SHR rd, 64-src_bits
                     *   - Mismo ancho (CAST/BITCAST entre tipos same-width):
                     *       solo MOV.
                     * El SHL+SAR/SHR mantiene los bits altos correctos
                     * para que ${var} (que lee qword) interprete bien
                     * el valor signed/unsigned.  Sin SIMD, sin tablas
                     * de mascaras: 2-3 instrs maquinas en el peor caso. */
                    case IrOp::CAST:
                    case IrOp::ZEXT:
                    case IrOp::SEXT:
                    case IrOp::TRUNC:
                    case IrOp::BITCAST: {
                        if (ins.operands.empty()) break;
                        if (ins.dst == ir::IR_NO_VALUE) break;
                        const ir::IrType src_t = ir_fn.values[ins.operands[0]].type;
                        const ir::IrType dst_t = ins.type;
                        const uint64_t src_bytes = ir_type_size_bytes(src_t);
                        const uint64_t dst_bytes = ir_type_size_bytes(dst_t);
                        const bool dst_signed = ir_type_is_signed_int(dst_t);
                        const bool src_signed = ir_type_is_signed_int(src_t);

                        /* Cargar src a SCRATCH_A (RAX). */
                        load_op(mf, ins.operands[0], SCRATCH_A);

                        if (dst_bytes < src_bytes) {
                            /* TRUNCATE: shift left + shift right segun signo
                             * del destino para extender bit de signo o cero. */
                            const int dst_bits = static_cast<int>(dst_bytes) * 8;
                            if (dst_bits < 64) {
                                const int shift = 64 - dst_bits;
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::SHL,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(shift)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(dst_signed ? MOp::SAR : MOp::SHR,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(shift)));
                            }
                        } else if (dst_bytes > src_bytes) {
                            /* WIDEN: shift segun signo de la FUENTE. */
                            const int src_bits = static_cast<int>(src_bytes) * 8;
                            if (src_bits < 64) {
                                const int shift = 64 - src_bits;
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::SHL,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(shift)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(src_signed ? MOp::SAR : MOp::SHR,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(shift)));
                            }
                        }
                        /* Mismo ancho: solo el MOV inicial; ya esta en RAX. */

                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- LOAD / STORE --------- */
                    /* Bug fix 2026-05-16: LOAD/STORE deben respetar el ancho
                     * del tipo (i32/i16/i8), no SIEMPRE qword.  El encoder
                     * solo soporta MOV qword, asi que emulamos:
                     *   LOAD: mov rax, qword [mem]; trunc/sign-extend a tipo.
                     *   STORE: trunc/sign-extend src a tipo; mov [mem], rax.
                     * Sin esto, ALLOCA 8 con STORE i32 deja upper bits con
                     * basura del valor anterior, y LOAD i32 los lee como
                     * parte del int -> wrong result (e.g. 0x200000002a en
                     * vez de 0x2a). */
                    case IrOp::LOAD: {
                        if (ins.operands.empty()) break;
                        load_op(mf, ins.operands[0], SCRATCH_B);
                        const uint64_t lbytes = ir_type_size_bytes(ins.type);
                        const bool lsigned = ir_type_is_signed_int(ins.type);
                        /* Emit native MOVZX/MOVSX para extension en 1 instr
                         * (en lugar de mov dword + SHL+SHR/SAR).  Reglas:
                         *   - i8/i16/i32 signed: MOVSX r64, r/m<w>
                         *   - u8/u16: MOVZX r64, r/m<w>
                         *   - u32: mov r32, r/m32  (zero-extend implicit por x86-64)
                         *   - i64/u64: mov r64, r/m64 */
                        if (lbytes == 0 || lbytes >= 8) {
                            /* qword normal. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A, 8),
                                    MOperand::make_mem(SCRATCH_B, 0)));
                        } else if (lsigned) {
                            /* MOVSX r64, r/m<w>.  Uso flags del MEM operand
                             * para indicar el ancho del source (no width,
                             * que ya guarda packed index/scale). */
                            MOperand mem_op = MOperand::make_mem(SCRATCH_B, 0);
                            mem_op.flags = static_cast<uint8_t>(lbytes);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOVSX,
                                    MOperand::make_reg(SCRATCH_A, 8),
                                    mem_op));
                        } else if (lbytes == 4) {
                            /* u32: mov r32 zero-extends. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A, 4),
                                    MOperand::make_mem(SCRATCH_B, 0)));
                        } else {
                            /* u8/u16: MOVZX r64, r/m<w>.  Mismo truco con flags. */
                            MOperand mem_op = MOperand::make_mem(SCRATCH_B, 0);
                            mem_op.flags = static_cast<uint8_t>(lbytes);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOVZX,
                                    MOperand::make_reg(SCRATCH_A, 8),
                                    mem_op));
                        }
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::STORE: {
                        if (ins.operands.size() < 2) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        load_op(mf, ins.operands[1], SCRATCH_B);

                        const uint64_t sbytes = ir_type_size_bytes(ins.type);
                        /* Encoder ahora soporta MOV con width 1/2/4/8.
                         * Solo necesitamos seteear el width del src reg. */
                        const uint8_t w = (sbytes == 1 || sbytes == 2
                                        || sbytes == 4 || sbytes == 8)
                                        ? static_cast<uint8_t>(sbytes) : 8;
                        MOperand src_op = MOperand::make_reg(SCRATCH_A, w);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(SCRATCH_B, 0),
                                src_op));
                        break;
                    }

                    /* --------- Comparaciones --------- */
                    case IrOp::CMP_EQ:
                    case IrOp::CMP_NE:
                    case IrOp::CMP_LT:
                    case IrOp::CMP_GT:
                    case IrOp::CMP_LE:
                    case IrOp::CMP_GE:
                    case IrOp::CMP_ULT:
                    case IrOp::CMP_UGT:
                    case IrOp::CMP_ULE:
                    case IrOp::CMP_UGE: {
                        if (ins.operands.size() < 2) break;
                        /* Patron correcto: zero RAX PRIMERO (sin daniar
                         * flags posteriores), luego CMP, luego SETCC.
                         * Si invertimos orden (CMP + XOR), el XOR
                         * destruye las flags antes de SETCC. */
                        /* xor rax, rax (zero rax: flags se descartan) */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::XOR,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_A)));
                        /* Cargar operands en RCX/RDX (no tocan flags). */
                        load_op(mf, ins.operands[0], SCRATCH_B);
                        load_op(mf, ins.operands[1], SCRATCH_C);
                        /* cmp rcx, rdx (sets flags). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CMP,
                                MOperand::make_reg(SCRATCH_B),
                                MOperand::make_reg(SCRATCH_C)));
                        /* setcc al (RAX upper bits ya estan a 0). */
                        MInstr setcc{};
                        setcc.op = MOp::SETCC;
                        setcc.variant = static_cast<uint8_t>(cond_for_cmp_op(ins.op));
                        setcc.dst = MOperand::make_reg(SCRATCH_A, 1);
                        mf.blocks.back().instrs.push_back(setcc);
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    /* --------- Calls --------- */
                    case IrOp::CALL: {
                        /* D.3-B: resolver el func_name a una direccion runtime. */
                        uint64_t fn_addr =
                            resolve_runtime_entry(ins.func_name, opts_.runtime);

                        /* D.3-E: si no es runtime entry, intentar resolver
                         * como funcion user (definida en el .velb).  El
                         * callback resuelve via lookup en ir_lookup +
                         * recursive eager-compile. */
                        bool is_user_call = false;
                        if (fn_addr == 0 && opts_.resolve_user_fn) {
                            fn_addr = opts_.resolve_user_fn(ins.func_name);
                            if (fn_addr != 0) is_user_call = true;
                        }

                        if (fn_addr == 0) {
                            /* No resolvido - marcar unsupported. */
                            if (jit::g_jit_warn_unsupported) {
                                /* Dedup por (op, line) -- callsites repetidos a
                                 * la misma entry solo se reportan una vez. */
                                auto key = std::make_pair(
                                    static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: CALL a entry '%s' no resuelto en fn '%s' linea %u\n",
                                        ins.func_name.c_str(),
                                        ir_fn.name.c_str(),
                                        ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }

                        /* Si es user call: marshalling VM_ABI (args a
                         * proc->registers.regs[1..N+1]) + arg0=proc.
                         * El return value se lee de regs[0]. */
                        if (is_user_call && opts_.mode == SelectorMode::VM_ABI) {
                            const size_t nargs = ins.operands.size();
                            const int32_t regs_base = static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET);

                            /* Paso 1: stage args en regs[1..N]. */
                            for (size_t a = 0; a < nargs && a < 12; ++a) {
                                load_op(mf, ins.operands[a], SCRATCH_A);
                                const int32_t off = regs_base + static_cast<int32_t>((a + 1) * 8);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, off),
                                        MOperand::make_reg(SCRATCH_A)));
                            }
                            /* Paso 2: R15 = nargs (calling convention). */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX, regs_base + 15 * 8),
                                    MOperand::make_imm32(static_cast<int32_t>(nargs))));
                            /* Paso 3: Native ABI arg0 = rbx (proc). */
#if defined(_WIN32)
                            const MReg arg0 = MReg::RCX;
#else
                            const MReg arg0 = MReg::RDI;
#endif
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(arg0),
                                    MOperand::make_reg(MReg::RBX)));
                            /* Paso 4: mov rax, fn_addr; call rax + stackmap. */
                            const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(fn_pool_idx)));
                            MInstr call_instr;
                            call_instr.op = MOp::CALL;
                            call_instr.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(call_instr);
                            mf.blocks.back().instrs.push_back(call_instr);
                            /* Paso 5: result en proc->registers.regs[0] -> dst. */
                            if (ins.dst != ir::IR_NO_VALUE) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_mem(MReg::RBX, regs_base)));
                                store_op(mf, ins.dst, SCRATCH_A);
                            }
                            break;
                        }

                        /* Native ABI arg regs (caller-saved). */
#if defined(_WIN32)
                        static const MReg ARG_REGS[] = {
                            MReg::RCX, MReg::RDX, MReg::R8, MReg::R9
                        };
                        constexpr size_t MAX_REG_ARGS = 4;
#else
                        static const MReg ARG_REGS[] = {
                            MReg::RDI, MReg::RSI, MReg::RDX,
                            MReg::RCX, MReg::R8, MReg::R9
                        };
                        constexpr size_t MAX_REG_ARGS = 6;
#endif
                        if (ins.operands.size() > MAX_REG_ARGS) {
                            /* Stack args no soportados aun en v1 selector. */
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(
                                    static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: CALL con %zu args (max %zu en regs) en fn '%s' linea %u\n",
                                        ins.operands.size(),
                                        MAX_REG_ARGS,
                                        ir_fn.name.c_str(),
                                        ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }

                        /* Marshalling: cargar cada arg en su native ABI reg.
                         * Como cada load_op solo escribe a un reg distinto,
                         * el orden no importa (sin clobber cross-arg). */
                        for (size_t a = 0; a < ins.operands.size(); ++a) {
                            load_op(mf, ins.operands[a], ARG_REGS[a]);
                        }

                        /* mov rax, fn_addr (via imm64 pool) */
                        const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(fn_pool_idx)));

                        /* Emitir stackmap para este call site (safepoint).
                         * El callee puede triggear GC -> walker captura RIP
                         * = return address (post-call).  Necesitamos
                         * stackmap con pc_offset <= post-call para que
                         * lookup_stackmap lo encuentre. */
                        MInstr call_instr;
                        call_instr.op = MOp::CALL;
                        call_instr.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(call_instr);
                        mf.blocks.back().instrs.push_back(call_instr);

                        /* Resultado en RAX -> slot del dst.  Si dst es
                         * GC, anyade el slot a live_gc_slots para futuros
                         * stackmaps. */
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* --------- CALLN (FFI a plugin nativo) --------- */
                    /*
                     * %dst = calln.T @"lib:func"(%arg0, %arg1, ...)
                     *
                     * Resolvemos el simbolo nativo en compile-time via
                     * @c opts_.resolve_native_fn (Loader provee la lambda
                     * que usa FFI::load_native_module + resolve_native_symbol).
                     * El resultado es un fn_ptr HOST que embebemos como
                     * imm64 + emitimos CALL nativo con Native ABI.
                     *
                     * Calling convention de las funciones nativas Vesta:
                     *   - Win64: rcx, rdx, r8, r9 (max 4 args en regs)
                     *   - SysV:  rdi, rsi, rdx, rcx, r8, r9 (max 6)
                     *   - Return en RAX
                     */
                    /* --------- NEWOBJ (instanciacion GC) --------- */
                    /*
                     * %dst = newobj.i64 %class_ptr
                     *
                     * Convencion del IR: dst = GcHandle (uint64).
                     * Pasos:
                     *   1. host_ptr = vrt_newobj(proc, class_ptr_slot)
                     *   2. handle   = vrt_gc_handle_for_ptr(proc, host_ptr)
                     *   3. slot[dst] = handle
                     *
                     * Mismo flujo que el mini-parser de raw_asm para
                     * `newobj rN` (D.3-G).
                     */
                    case IrOp::NEWOBJ: {
                        /* Optimizado 2026-05-16: usar vrt_newobj_handle (1 call
                         * que combina alloc + handle_for_ptr) en lugar de los
                         * 2 calls separados.  Ahorra ~30-50 ns por @c new X(). */
                        if (ins.operands.empty()) break;
                        if (opts_.runtime == nullptr
                         || opts_.runtime->newobj_handle == nullptr) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->newobj_handle no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        if (opts_.mode != SelectorMode::VM_ABI) {
                            warn_unsupported(ins.op, ins.source_line,
                                "NEWOBJ requiere VM_ABI");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
#if defined(_WIN32)
                        const MReg ABI0 = MReg::RCX;
                        const MReg ABI1 = MReg::RDX;
#else
                        const MReg ABI0 = MReg::RDI;
                        const MReg ABI1 = MReg::RSI;
#endif
                        /* vrt_newobj_handle(proc, cls) -> handle en RAX. */
                        load_op(mf, ins.operands[0], ABI1);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ABI0),
                                MOperand::make_reg(MReg::RBX)));
                        const uint32_t pidx = mf.intern_imm64(
                            reinterpret_cast<uint64_t>(opts_.runtime->newobj_handle));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(pidx)));
                        MInstr c1;
                        c1.op = MOp::CALL;
                        c1.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(c1);
                        mf.blocks.back().instrs.push_back(c1);
                        /* slot[dst] = handle (en RAX). */
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    case IrOp::CALLN: {
                        if (opts_.resolve_native_fn == nullptr) {
                            warn_unsupported(ins.op, ins.source_line,
                                "resolve_native_fn no provisto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        uint64_t fn_addr = opts_.resolve_native_fn(ins.func_name);
                        if (fn_addr == 0) {
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: CALLN '%s' no se pudo resolver en fn '%s' linea %u\n",
                                        ins.func_name.c_str(),
                                        ir_fn.name.c_str(),
                                        ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }

                        /* Native ABI arg regs. */
#if defined(_WIN32)
                        static const MReg N_ARG_REGS[] = {
                            MReg::RCX, MReg::RDX, MReg::R8, MReg::R9
                        };
                        constexpr size_t N_MAX_REG_ARGS = 4;
#else
                        static const MReg N_ARG_REGS[] = {
                            MReg::RDI, MReg::RSI, MReg::RDX,
                            MReg::RCX, MReg::R8, MReg::R9
                        };
                        constexpr size_t N_MAX_REG_ARGS = 6;
#endif
                        if (ins.operands.size() > N_MAX_REG_ARGS) {
                            warn_unsupported(ins.op, ins.source_line,
                                "CALLN con demasiados args (stack args no implementados)");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }

                        /* Cargar cada operando al reg correspondiente.
                         * Sin clobber porque cada reg es distinto. */
                        for (size_t a = 0; a < ins.operands.size(); ++a) {
                            load_op(mf, ins.operands[a], N_ARG_REGS[a]);
                        }

                        /* mov rax, fn_addr (via imm64 pool). */
                        const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(fn_pool_idx)));

                        /* CALL rax + stackmap (el plugin nativo podria
                         * triggear GC indirectamente). */
                        MInstr call_instr;
                        call_instr.op = MOp::CALL;
                        call_instr.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(call_instr);
                        mf.blocks.back().instrs.push_back(call_instr);

                        /* Resultado en RAX -> slot del dst. */
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* --------- CALLVIRT (dispatch dinamico v1 via runtime) --------- */
                    /*
                     * Despachamos a un runtime entry @c vrt_callvirt que hace
                     * el vtable lookup + dispatch:
                     *   1. Colocar args en proc->registers.regs[1..N+1]
                     *      (VM_ABI convention).  r1 = obj, r2..r_{N+1} = args.
                     *   2. CALL vrt_callvirt(proc, obj_payload, vtbl_idx)
                     *      con Native ABI args (rdi/rcx=proc, rsi/rdx=obj,
                     *      rdx/r8=vtbl_idx).
                     *   3. Resultado en RAX -> dst slot.
                     *
                     * Si la callee aun no tiene jit_code, vrt_callvirt intenta
                     * compilarla on-the-fly.  Si falla, lanza FatalError
                     * capturable.  Sin fallback automatico a interp en v1
                     * (necesita un trampoline jit_to_interp completo, Phase D.3-E).
                     */
                    case IrOp::CALLVIRT: {
                        if (ins.operands.empty()) break;
                        if (opts_.runtime == nullptr || opts_.runtime->callvirt == nullptr) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->callvirt no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint32_t vtbl_idx = static_cast<uint32_t>(ins.imm);
                        const size_t   nargs    = ins.operands.size() > 1
                                                ? std::min(ins.operands.size() - 1, size_t(11))
                                                : 0;

                        /* INLINE CALLVIRT DISPATCH.
                         *
                         * En vez de llamar siempre a vrt_callvirt (que tiene
                         * mutex + checks + bridge overhead), emitimos un
                         * dispatch inline:
                         *   1. Setup args en proc->registers.regs[1..N+1] y R15.
                         *   2. Inline check: si method->jit_code != null -> direct call.
                         *   3. Si null (no compilado), fall to vrt_callvirt slow path.
                         *
                         * Esto reduce el callvirt JIT de ~50ns (vrt_callvirt
                         * con sus overheads) a ~10ns (5 loads + branch + call).
                         * Para benchmarks callvirt-heavy esto es 3-5x speedup. */
                        const int32_t regs_base = static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET);

                        /* Paso 1: escribir args a proc->registers.regs[1..N+1].
                         * regs[1] = obj (operands[0]); regs[2..N+1] = args. */
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 8),
                                MOperand::make_reg(SCRATCH_A)));
                        for (size_t a = 0; a < nargs; ++a) {
                            load_op(mf, ins.operands[a + 1], SCRATCH_A);
                            const int32_t off = regs_base + static_cast<int32_t>((a + 2) * 8);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX, off),
                                    MOperand::make_reg(SCRATCH_A)));
                        }

                        /* Paso 2: setear R15 = nargs + 1 (calling convention CALLVIRT). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 15 * 8),
                                MOperand::make_imm32(static_cast<int32_t>(nargs + 1))));

                        /* INLINE CACHE monomorphic (si reserve_ic_slot disponible).
                         *
                         * Patron:
                         *   load obj -> rax
                         *   test rax,rax; jz fallback_null
                         *   mov rcx, [rax+0]               ; class_ptr
                         *   mov r10, ic_slot_addr           ; imm64
                         *   cmp rcx, [r10]                  ; cached_class
                         *   jne ic_miss
                         *   mov rax, [r10+8]                ; cached_jit_code
                         *   mov rcx, rbx                    ; proc
                         *   call rax
                         *   jmp continue
                         * ic_miss:
                         *   mov arg0=rbx, arg1=rax(obj), arg2=vtbl_idx, arg3=ic_slot
                         *   call vrt_callvirt_ic
                         *   jmp continue
                         * fallback_null:
                         *   ... null obj path (raro) ...
                         *
                         * Ventaja vs inline dispatch tradicional: hit elimina
                         * 4 loads (vtable+method+jit_code+advice).  Coste por
                         * hit ~5 instr (load class + cmp + branch + call).
                         * Coste por miss ~vrt_callvirt_ic overhead (~50ns) +
                         * actualizar slot (incluido en vrt_callvirt_ic).
                         */
                        uint64_t ic_slot_addr = 0;
                        if (opts_.reserve_ic_slot) {
                            ic_slot_addr = opts_.reserve_ic_slot();
                        }
                        if (ic_slot_addr != 0 && opts_.runtime
                         && opts_.runtime->callvirt_ic) {
                            const MLabelId ic_miss_label = mf.new_label();
                            const MLabelId ic_continue_label = mf.new_label();
                            /* load obj */
                            load_op(mf, ins.operands[0], MReg::RAX);
                            /* mov rcx, [rax+0]  (class_ptr) */
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_mem(MReg::RAX, 0)));
                            /* mov r10, ic_slot_addr (imm64 via pool) */
                            const uint32_t ic_idx = mf.intern_imm64(ic_slot_addr);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R10),
                                MOperand::make_imm64_idx(ic_idx)));
                            /* cmp rcx, [r10] (cached_class) */
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::CMP,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_mem(MReg::R10, 0)));
                            /* jne ic_miss */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_jcc(MCond::NE, ic_miss_label));
                            /* HIT: mov rax, [r10+8] (cached_jit_code) */
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_mem(MReg::R10, 8)));
                            /* mov rcx, rbx (proc) */
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_reg(MReg::RBX)));
                            /* call rax + stackmap */
                            {
                                MInstr hit_call;
                                hit_call.op = MOp::CALL;
                                hit_call.src1 = MOperand::make_reg(MReg::RAX);
                                emit_stackmap_for_safepoint(hit_call);
                                mf.blocks.back().instrs.push_back(hit_call);
                            }
                            /* jmp ic_continue */
                            mf.blocks.back().instrs.push_back(MInstr::make_jmp(ic_continue_label));
                            /* ic_miss: */
                            mf.blocks.back().instrs.push_back(MInstr::make_label_def(ic_miss_label));
                            /* Setup args para vrt_callvirt_ic(proc, obj, idx, slot). */
                            load_op(mf, ins.operands[0], MReg::RAX);  /* obj */
#if defined(_WIN32)
                            const MReg ic_arg0 = MReg::RCX;  /* proc */
                            const MReg ic_arg1 = MReg::RDX;  /* obj */
                            const MReg ic_arg2 = MReg::R8;   /* vtbl_idx */
                            const MReg ic_arg3 = MReg::R9;   /* ic_slot */
#else
                            const MReg ic_arg0 = MReg::RDI;
                            const MReg ic_arg1 = MReg::RSI;
                            const MReg ic_arg2 = MReg::RDX;
                            const MReg ic_arg3 = MReg::RCX;
#endif
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ic_arg0),
                                MOperand::make_reg(MReg::RBX)));
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ic_arg1),
                                MOperand::make_reg(MReg::RAX)));
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ic_arg2),
                                MOperand::make_imm32(static_cast<int32_t>(vtbl_idx))));
                            const uint32_t slot_idx2 = mf.intern_imm64(ic_slot_addr);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ic_arg3),
                                MOperand::make_imm64_idx(slot_idx2)));
                            const uint64_t ic_fn = reinterpret_cast<uint64_t>(opts_.runtime->callvirt_ic);
                            const uint32_t ic_fn_idx = mf.intern_imm64(ic_fn);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(ic_fn_idx)));
                            {
                                MInstr miss_call;
                                miss_call.op = MOp::CALL;
                                miss_call.src1 = MOperand::make_reg(MReg::RAX);
                                emit_stackmap_for_safepoint(miss_call);
                                mf.blocks.back().instrs.push_back(miss_call);
                            }
                            /* ic_continue: */
                            mf.blocks.back().instrs.push_back(MInstr::make_label_def(ic_continue_label));
                            /* Store result en proc->registers.regs[0] -> dst. */
                            if (ins.dst != ir::IR_NO_VALUE) {
                                mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_mem(MReg::RBX, regs_base)));
                                store_op(mf, ins.dst, SCRATCH_A);
                            }
                            break;
                        }

                        /* Paso 3 INLINE DISPATCH (sin IC):
                         *   load obj -> rax
                         *   test rax,rax; jz fallback
                         *   mov rcx, [rax + 0]        ; class_ptr (header @ offset 0)
                         *   test rcx,rcx; jz fallback
                         *   mov rcx, [rcx + 80]       ; vtable
                         *   test rcx,rcx; jz fallback
                         *   mov rcx, [rcx + idx*8]    ; method
                         *   test rcx,rcx; jz fallback
                         *   mov rax, [rcx + 104]      ; jit_code
                         *   test rax,rax; jz fallback
                         *   mov rcx, rbx              ; arg0 = proc
                         *   call rax                  ; direct
                         *   jmp continue
                         * fallback:
                         *   ; full vrt_callvirt slow path
                         * continue: */
                        const MLabelId fallback_label = mf.new_label();
                        const MLabelId continue_label = mf.new_label();

                        /* Load obj a RAX. */
                        load_op(mf, ins.operands[0], MReg::RAX);
                        /* test rax,rax; jz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, fallback_label));
                        /* mov rcx, [rax + 0]  (class_ptr) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_mem(MReg::RAX, 0)));
                        /* test rcx,rcx; jz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_reg(MReg::RCX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, fallback_label));
                        /* mov rcx, [rcx + VTABLE_OFFSET]  (vtable) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_mem(MReg::RCX, VESTA_CLASSINFO_VTABLE_OFFSET)));
                        /* test rcx,rcx; jz fallback (no vtable = abstract o vtable_size=0) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_reg(MReg::RCX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, fallback_label));
                        /* mov rcx, [rcx + vtbl_idx*8]  (method) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_mem(MReg::RCX,
                                static_cast<int32_t>(vtbl_idx * 8))));
                        /* test rcx,rcx; jz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_reg(MReg::RCX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, fallback_label));
                        /* AOP fix 2026-05-16: si method->advice_chain != NULL,
                         * el metodo tiene aspectos (@Before/@After/@Around).
                         * Saltar el fast path inline (que invocaria solo el
                         * body sin advices) y caer al slow path vrt_callvirt
                         * que recorre la cadena correctamente.
                         *
                         *   mov rax, [rcx + ADVICE_CHAIN_OFFSET]
                         *   test rax, rax; jnz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RCX, VESTA_METHODINFO_ADVICE_CHAIN_OFFSET)));
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::NE, fallback_label));
                        /* mov rax, [rcx + JIT_CODE_OFFSET]  (method->jit_code) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RCX, VESTA_METHODINFO_JIT_CODE_OFFSET)));
                        /* test rax,rax; jz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, fallback_label));
                        /* FAST PATH: mov rcx, rbx; call rax */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_reg(MReg::RBX)));
                        {
                            MInstr fast_call;
                            fast_call.op = MOp::CALL;
                            fast_call.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(fast_call);
                            mf.blocks.back().instrs.push_back(fast_call);
                        }
                        /* jmp continue */
                        mf.blocks.back().instrs.push_back(MInstr::make_jmp(continue_label));

                        /* fallback_label: */
                        mf.blocks.back().instrs.push_back(MInstr::make_label_def(fallback_label));

                        /* SLOW PATH: vrt_callvirt(proc, obj, vtbl_idx). */
                        load_op(mf, ins.operands[0], SCRATCH_A);  /* obj host_ptr */
#if defined(_WIN32)
                        const MReg arg0 = MReg::RCX;
                        const MReg arg1 = MReg::RDX;
                        const MReg arg2 = MReg::R8;
#else
                        const MReg arg0 = MReg::RDI;
                        const MReg arg1 = MReg::RSI;
                        const MReg arg2 = MReg::RDX;
#endif
                        /* mov arg0, rbx (proc) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0),
                            MOperand::make_reg(MReg::RBX)));
                        /* mov arg1, SCRATCH_A (obj) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg1),
                            MOperand::make_reg(SCRATCH_A)));
                        /* mov arg2, vtbl_idx */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg2),
                            MOperand::make_imm32(static_cast<int32_t>(vtbl_idx))));
                        /* mov rax, vrt_callvirt; call rax */
                        const uint64_t fn_addr = reinterpret_cast<uint64_t>(
                            opts_.runtime->callvirt);
                        const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_pool_idx)));
                        {
                            MInstr slow_call;
                            slow_call.op = MOp::CALL;
                            slow_call.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(slow_call);
                            mf.blocks.back().instrs.push_back(slow_call);
                        }

                        /* continue_label: */
                        mf.blocks.back().instrs.push_back(MInstr::make_label_def(continue_label));

                        /* Paso 5: el return value esta en RAX (tanto fast path
                         * como slow path lo dejaron alli).  Store al slot del dst. */
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* --------- CALLM (dispatch via MethodInfo*) --------- */
                    /* Layout IR: operands[0]=obj, operands[1]=method_ptr,
                     * operands[2..]=args.  Reusa vrt_callm runtime entry. */
                    case IrOp::CALLM: {
                        if (ins.operands.size() < 2) break;
                        if (opts_.runtime == nullptr || opts_.runtime->callm == nullptr) {
                            warn_unsupported(ins.op, ins.source_line, "runtime->callm null");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const size_t   nargs     = ins.operands.size() > 2
                                                 ? std::min(ins.operands.size() - 2, size_t(11))
                                                 : 0;
                        const int32_t  regs_base = static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET);

                        /* Stage args: regs[1]=obj, regs[2..N+1]=args. */
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 8),
                                MOperand::make_reg(SCRATCH_A)));
                        for (size_t a = 0; a < nargs; ++a) {
                            load_op(mf, ins.operands[a + 2], SCRATCH_A);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX,
                                        regs_base + static_cast<int32_t>((a + 2) * 8)),
                                    MOperand::make_reg(SCRATCH_A)));
                        }
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 15 * 8),
                                MOperand::make_imm32(static_cast<int32_t>(nargs + 1))));

                        /* Native ABI: arg0=proc(rbx), arg1=obj_payload, arg2=method_ptr.
                         *
                         * BUG FIX 2026-05-16: usar R10/R11 (caller-saved, no
                         * arg regs) como temporales para evitar clobber.  El
                         * codigo anterior usaba SCRATCH_C=RDX que ES cm_arg1
                         * (Win64) o cm_arg2 (SysV) -- al mov-ear arg1 desde
                         * RAX, sobrescribia RDX (method) antes de pasarlo
                         * como arg2.  Resultado: vrt_callm(proc, obj, obj)
                         * en vez de (proc, obj, method) -> retornaba 0. */
                        load_op(mf, ins.operands[0], MReg::R10);  /* obj host_ptr */
                        load_op(mf, ins.operands[1], MReg::R11);  /* method ptr */
#if defined(_WIN32)
                        const MReg cm_arg0 = MReg::RCX;
                        const MReg cm_arg1 = MReg::RDX;
                        const MReg cm_arg2 = MReg::R8;
#else
                        const MReg cm_arg0 = MReg::RDI;
                        const MReg cm_arg1 = MReg::RSI;
                        const MReg cm_arg2 = MReg::RDX;
#endif
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cm_arg0),
                                MOperand::make_reg(MReg::RBX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cm_arg1),
                                MOperand::make_reg(MReg::R10)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cm_arg2),
                                MOperand::make_reg(MReg::R11)));

                        const uint64_t fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->callm);
                        const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(fn_pool_idx)));
                        MInstr cm_call;
                        cm_call.op = MOp::CALL;
                        cm_call.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(cm_call);
                        mf.blocks.back().instrs.push_back(cm_call);
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* --------- CALLCLOSURE --------- */
                    /* Layout IR: func_ptr (en ins.func_ptr SSA), operands[0]=env,
                     * operands[1..]=args.  Reusa vrt_callclosure runtime entry. */
                    case IrOp::CALLCLOSURE: {
                        if (ins.operands.empty()) break;
                        if (opts_.runtime == nullptr || opts_.runtime->callclosure == nullptr) {
                            warn_unsupported(ins.op, ins.source_line, "runtime->callclosure null");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        if (ins.func_ptr == ir::IR_NO_VALUE) {
                            warn_unsupported(ins.op, ins.source_line, "func_ptr ausente");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* operands[0] = env_addr; operands[1..] = args. */
                        const size_t   nargs     = ins.operands.size() > 1
                                                 ? std::min(ins.operands.size() - 1, size_t(12))
                                                 : 0;
                        const int32_t  regs_base = static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET);

                        /* Stage args: regs[1..N]=args. */
                        for (size_t a = 0; a < nargs; ++a) {
                            load_op(mf, ins.operands[a + 1], SCRATCH_A);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX,
                                        regs_base + static_cast<int32_t>((a + 1) * 8)),
                                    MOperand::make_reg(SCRATCH_A)));
                        }
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 15 * 8),
                                MOperand::make_imm32(static_cast<int32_t>(nargs))));

                        /* Native ABI: arg0=proc(rbx), arg1=fn_addr, arg2=env_addr.
                         *
                         * BUG FIX 2026-05-16: usar R10/R11 (caller-saved, no
                         * arg regs) como temporales para evitar clobber.
                         * Mismo issue que CALLM: SCRATCH_C=RDX colisiona con
                         * cc_arg1(Win64)/cc_arg2(SysV). */
                        load_op(mf, ins.func_ptr,    MReg::R10);   /* fn_addr */
                        load_op(mf, ins.operands[0], MReg::R11);   /* env_addr */
#if defined(_WIN32)
                        const MReg cc_arg0 = MReg::RCX;
                        const MReg cc_arg1 = MReg::RDX;
                        const MReg cc_arg2 = MReg::R8;
#else
                        const MReg cc_arg0 = MReg::RDI;
                        const MReg cc_arg1 = MReg::RSI;
                        const MReg cc_arg2 = MReg::RDX;
#endif
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cc_arg0),
                                MOperand::make_reg(MReg::RBX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cc_arg1),
                                MOperand::make_reg(MReg::R10)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(cc_arg2),
                                MOperand::make_reg(MReg::R11)));

                        const uint64_t fn_addr_cc = reinterpret_cast<uint64_t>(opts_.runtime->callclosure);
                        const uint32_t fn_pool_cc = mf.intern_imm64(fn_addr_cc);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(fn_pool_cc)));
                        MInstr cc_call;
                        cc_call.op = MOp::CALL;
                        cc_call.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(cc_call);
                        mf.blocks.back().instrs.push_back(cc_call);
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* --------- Control de flujo --------- */
                    case IrOp::BR: {
                        /* PHI elimination via copies at predecessor.
                         * Para cada PHI en el target_block, emitir un copy
                         * del valor entrante del bloque actual al slot del
                         * PHI dst.  Asi cuando control llega al target, el
                         * slot del PHI ya tiene el valor correcto. */
                        if (ins.target_block < ir_fn.blocks.size()) {
                            const auto &target = ir_fn.blocks[ins.target_block];
                            for (const auto &pin : target.instrs) {
                                if (pin.op != ir::IrOp::PHI) break;  /* PHIs solo al inicio */
                                /* Buscar el phi_arg que corresponde al bloque actual. */
                                for (const auto &arg : pin.phi_args) {
                                    if (arg.block == static_cast<ir::IrBlockId>(bi)) {
                                        load_op(mf, arg.value, SCRATCH_A);
                                        store_op(mf, pin.dst, SCRATCH_A);
                                        break;
                                    }
                                }
                            }
                        }
                        /* si el target es un back-edge (target <= current),
                         * insertar SAFEPOINT poll antes del JMP. */
                        if (opts_.mode == SelectorMode::VM_ABI
                         && ins.target_block <= static_cast<ir::IrBlockId>(bi)
                         && safepoint_pool_idx_ != UINT32_MAX) {
                            MInstr sp_instr = MInstr::make_safepoint(safepoint_pool_idx_);
                            emit_stackmap_for_safepoint(sp_instr);
                            mf.blocks.back().instrs.push_back(sp_instr);
                        }
                        if (ins.target_block < block_labels.size()) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_jmp(block_labels[ins.target_block]));
                        }
                        break;
                    }
                    case IrOp::BR_COND: {
                        if (ins.operands.empty()) break;
                        load_op(mf, ins.operands[0], SCRATCH_A);
                        /* SAFEPOINT antes del condicional si alguna rama es back-edge. */
                        const bool is_back =
                            opts_.mode == SelectorMode::VM_ABI &&
                            ((ins.target_block <= static_cast<ir::IrBlockId>(bi)) ||
                             (ins.false_block  <= static_cast<ir::IrBlockId>(bi)));
                        if (is_back && safepoint_pool_idx_ != UINT32_MAX) {
                            MInstr sp_instr = MInstr::make_safepoint(safepoint_pool_idx_);
                            emit_stackmap_for_safepoint(sp_instr);
                            mf.blocks.back().instrs.push_back(sp_instr);
                        }

                        /* PHI elimination con cond branching.
                         * Detectar si los targets tienen PHIs que requieran
                         * copies distintos.  Si NO hay PHIs, fast path con
                         * el codigo simple.  Si SI, fork por rama. */
                        auto target_has_phi = [&](ir::IrBlockId tb) -> bool {
                            if (tb >= ir_fn.blocks.size()) return false;
                            for (const auto &pin : ir_fn.blocks[tb].instrs) {
                                if (pin.op != ir::IrOp::PHI) return false;
                                /* Hay al menos un PHI con phi_arg desde bi. */
                                for (const auto &arg : pin.phi_args) {
                                    if (arg.block == static_cast<ir::IrBlockId>(bi)) return true;
                                }
                            }
                            return false;
                        };

                        const bool t_has_phi = target_has_phi(ins.target_block);
                        const bool f_has_phi = target_has_phi(ins.false_block);

                        auto emit_phi_copies_for_target = [&](ir::IrBlockId tb) {
                            if (tb >= ir_fn.blocks.size()) return;
                            for (const auto &pin : ir_fn.blocks[tb].instrs) {
                                if (pin.op != ir::IrOp::PHI) break;
                                for (const auto &arg : pin.phi_args) {
                                    if (arg.block == static_cast<ir::IrBlockId>(bi)) {
                                        load_op(mf, arg.value, SCRATCH_A);
                                        store_op(mf, pin.dst, SCRATCH_A);
                                        break;
                                    }
                                }
                            }
                        };

                        /* test rax, rax */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::TEST,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_A)));

                        if (!t_has_phi && !f_has_phi) {
                            /* Fast path: no PHIs, codigo identico al original. */
                            if (ins.target_block < block_labels.size()) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(MCond::NE,
                                        block_labels[ins.target_block]));
                            }
                            if (ins.false_block < block_labels.size()) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jmp(block_labels[ins.false_block]));
                            }
                        } else {
                            /* Long form: JE saltar PHIs del taken; emitir
                             *            PHI copies + JMP target.
                             *            Label fallback: PHIs false + JMP false. */
                            const MLabelId skip_taken_lbl = mf.new_label();
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_jcc(MCond::E, skip_taken_lbl));
                            /* PHIs del taken */
                            emit_phi_copies_for_target(ins.target_block);
                            if (ins.target_block < block_labels.size()) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jmp(block_labels[ins.target_block]));
                            }
                            /* Label intermedio: emite LABEL_DEF para que el
                             * encoder registre su offset. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_label_def(skip_taken_lbl));
                            /* PHIs del false target */
                            emit_phi_copies_for_target(ins.false_block);
                            if (ins.false_block < block_labels.size()) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jmp(block_labels[ins.false_block]));
                            }
                        }
                        break;
                    }
                    case IrOp::RET: {
                        /* Cargar return value en RAX (si existe). */
                        if (!ins.operands.empty()
                         && ins.operands[0] != ir::IR_NO_VALUE) {
                            load_op(mf, ins.operands[0], MReg::RAX);
                        }
                        /* VM_ABI: ademas escribir RAX a proc->registers.regs[0]
                         * para que el caller bytecode/interprete vea el return.
                         * NATIVE_ABI: nada extra, return en RAX como convencion C. */
                        if (opts_.mode == SelectorMode::VM_ABI) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX,
                                        VESTA_PROC_REGISTERS_OFFSET),
                                    MOperand::make_reg(MReg::RAX)));
                        }
                        /* epilogue: mov rsp, rbp; pop rbp;
                         *           [pop r15;..;pop r12];   (regalloc)
                         *           [pop rbx];               (VM_ABI)
                         *           ret */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RSP),
                                MOperand::make_reg(MReg::RBP)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::POP,
                                MOperand::make_reg(MReg::RBP), {}));
                        /* pop regs callee-saved en orden
                         * inverso al push del prologue. */
                        for (auto it = regalloc.callee_saved_used.rbegin();
                             it != regalloc.callee_saved_used.rend(); ++it) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::POP,
                                    MOperand::make_reg(*it), {}));
                        }
                        if (opts_.mode == SelectorMode::VM_ABI) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::POP,
                                    MOperand::make_reg(MReg::RBX), {}));
                        }
                        mf.blocks.back().instrs.push_back(MInstr::make_ret());
                        break;
                    }

                    case ir::IrOp::RAW_ASM: {
                        /* RAW_ASM lleva texto assembly .vel en func_name.
                         * El JIT NO tiene un assembler embebido, asi que
                         * la unica forma de soportarlo es reconocer
                         * patrones estereotipados emitidos por el frontend
                         * y traducirlos a CALLs a runtime entries.
                         *
                         * Patrones soportados v1:
                         *   "gchandle {dst}, {src0}\n"
                         *     -> vrt_gc_handle_for_ptr(proc, src0) -> dst
                         *   "gcderef cur0, {src0}\nxchg cur0, {dst}\n"
                         *     -> vrt_gc_deref(proc, src0) -> dst
                         *
                         * Cualquier otro texto cae a unsupported.  El
                         * frontend emite estos dos patrones MASIVAMENTE
                         * (cualquier `new X()` los usa post-newobj para
                         * convertir GcHandle -> host_ptr; cualquier asignacion
                         * a field GC usa gchandle pre-store para
                         * convertir host_ptr -> GcHandle).
                         *
                         * is_call_site: si esta seteado, el bloque se
                         * comporta como llamada y debe preservar
                         * caller-saved regs.  Para gchandle/gcderef NO se
                         * setea normalmente (no triggea GC), pero el code
                         * por seguridad usa el mismo path stackmap que CALL. */
                        const std::string &asm_text = ins.func_name;
                        bool matched = false;

                        /* Helper interno: emite la secuencia call para
                         * vrt_gc_handle_for_ptr / vrt_gc_deref (misma forma). */
                        auto emit_call_2arg_runtime = [&](uint64_t fn_addr) {
#if defined(_WIN32)
                            const MReg arg0 = MReg::RCX;
                            const MReg arg1 = MReg::RDX;
#else
                            const MReg arg0 = MReg::RDI;
                            const MReg arg1 = MReg::RSI;
#endif
                            /* mov arg0, rbx (proc). */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(arg0),
                                    MOperand::make_reg(MReg::RBX)));
                            /* mov arg1, [slot[src0]]. */
                            load_op(mf, ins.operands[0], arg1);

                            /* mov rax, fn_addr (via imm64 pool). */
                            const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(fn_pool_idx)));

                            /* call rax + stackmap embebido. */
                            MInstr call_instr;
                            call_instr.op = MOp::CALL;
                            call_instr.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(call_instr);
                            mf.blocks.back().instrs.push_back(call_instr);

                            /* result en RAX -> dst slot. */
                            store_op(mf, ins.dst, MReg::RAX);
                        };

                        if (asm_text == "gchandle {dst}, {src0}\n"
                         && ins.operands.size() == 1
                         && ins.dst != ir::IR_NO_VALUE
                         && opts_.runtime != nullptr
                         && opts_.runtime->gc_handle_for_ptr != nullptr) {
                            emit_call_2arg_runtime(reinterpret_cast<uint64_t>(
                                opts_.runtime->gc_handle_for_ptr));
                            matched = true;
                        } else if (asm_text == "gcderef cur0, {src0}\nxchg cur0, {dst}\n"
                                && ins.operands.size() == 1
                                && ins.dst != ir::IR_NO_VALUE
                                && opts_.runtime != nullptr
                                && opts_.runtime->gc_deref != nullptr) {
                            emit_call_2arg_runtime(reinterpret_cast<uint64_t>(
                                opts_.runtime->gc_deref));
                            matched = true;
                        }

                        /* mini-parser de raw_asm para los
                         * patrones estereotipados del frontend.  Lo escribimos
                         * INLINE aqui (no como helper externo) para tener
                         * acceso a SCRATCH_A/B/C + mf + emit_stackmap_for_safepoint
                         * + opts_ sin necesidad de pasar todo via parametros.
                         *
                         * Estrategia:
                         *   1. Iterar linea por linea (split por '\n').
                         *   2. Tokenizar opcode + operandos (comma-separated).
                         *   3. Dispatch en cadena if/else por opcode.
                         *   4. Si cualquier linea falla, descartamos las
                         *      MInstrs staged y caemos a unsupported (sin
                         *      contaminacion del block actual).
                         *
                         * Casos cubiertos (todos VM_ABI):
                         *   subsp/addsp rN, imm    -> SUB/ADD [rbx + slot*8], imm
                         *   mov rN, rM             -> reg copy via SCRATCH
                         *   mov rN, imm            -> imm to slot
                         *   mov [rN], rM           -> vrt_vm_write_u64
                         *   mov [rN], imm          -> vrt_vm_write_u64
                         *   mov rN, [rM]           -> vrt_vm_read_u64
                         *   addu/adds/subu/subs rN, rM|imm
                         *   findclass rN, rM       -> vrt_findclass
                         *   newobj rN              -> vrt_newobj
                         *   defclass rN, rM        -> vrt_defclass
                         *   deffield rN, rM        -> vrt_deffield
                         *   defmethod rN, rM       -> vrt_defmethod
                         *   findmethod rN, rM      -> vrt_findmethod
                         *   findfield rN, rM       -> vrt_findfield
                         */
                        if (!matched
                         && opts_.mode == SelectorMode::VM_ABI
                         && opts_.runtime != nullptr
                         && opts_.runtime->vm_write_u64 != nullptr) {
                            std::vector<MInstr> staged;
                            bool all_ok = true;

                            /* Helper para emitir un call nativo a un runtime
                             * entry con N args.  args[] son MReg fuente que
                             * deben moverse a ABI argregs antes del call. */
                            auto stage_load_slot = [&](MReg dst, int slot) {
                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(dst),
                                    MOperand::make_mem(MReg::RBX, vm_reg_offset(slot))));
                            };
                            auto stage_store_slot = [&](int slot, MReg src) {
                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX, vm_reg_offset(slot)),
                                    MOperand::make_reg(src)));
                            };
                            auto stage_load_imm = [&](MReg dst, int64_t v) {
                                if (v >= INT32_MIN && v <= INT32_MAX) {
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(dst),
                                        MOperand::make_imm32(static_cast<int32_t>(v))));
                                } else {
                                    const uint32_t pi = mf.intern_imm64(static_cast<uint64_t>(v));
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(dst),
                                        MOperand::make_imm64_idx(pi)));
                                }
                            };

#if defined(_WIN32)
                            const MReg ABI_ARG0 = MReg::RCX;
                            const MReg ABI_ARG1 = MReg::RDX;
                            const MReg ABI_ARG2 = MReg::R8;
                            const MReg ABI_ARG3 = MReg::R9;
#else
                            const MReg ABI_ARG0 = MReg::RDI;
                            const MReg ABI_ARG1 = MReg::RSI;
                            const MReg ABI_ARG2 = MReg::RDX;
                            const MReg ABI_ARG3 = MReg::RCX;
#endif
                            /* Helper para emitir un CALL a runtime entry
                             * con stackmap.  args ya en arg regs ABI. */
                            auto stage_call = [&](uint64_t fn_addr) {
                                const uint32_t pi = mf.intern_imm64(fn_addr);
                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(pi)));
                                MInstr ci;
                                ci.op = MOp::CALL;
                                ci.src1 = MOperand::make_reg(MReg::RAX);
                                staged.push_back(ci);
                            };

                            /* pre-coleccionar lineas para
                             * permitir lookahead (necesario para el
                             * patron `gcderef cur0, rN` + `xchg cur0, rM`
                             * que el frontend genera siempre como par
                             * inseparable). */
                            std::vector<std::string> lines;
                            {
                                std::istringstream iss(asm_text);
                                std::string raw;
                                while (std::getline(iss, raw)) {
                                    raw = trim_str(raw);
                                    if (raw.empty() || raw.substr(0,2) == "//") continue;
                                    lines.push_back(std::move(raw));
                                }
                            }
                            for (size_t li = 0; li < lines.size() && all_ok; ++li) {
                                const std::string &line = lines[li];
                                size_t sp = line.find_first_of(" \t");
                                std::string opcode = (sp == std::string::npos)
                                                       ? line : line.substr(0, sp);
                                std::string rest = (sp == std::string::npos)
                                                       ? "" : trim_str(line.substr(sp));
                                auto args = split_csv(rest);

                                /* @c subsp/addsp rN, imm: modify VM reg by imm. */
                                if ((opcode == "subsp" || opcode == "addsp") && args.size() == 2) {
                                    const int slot = vm_reg_slot_index(args[0]);
                                    int64_t imm;
                                    if (slot < 0 || !parse_imm_int(args[1], imm)
                                     || imm < INT32_MIN || imm > INT32_MAX) {
                                        all_ok = false; break;
                                    }
                                    stage_load_slot(MReg::RAX, slot);
                                    staged.push_back(MInstr::make_unary(
                                        opcode == "subsp" ? MOp::SUB : MOp::ADD,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_imm32(static_cast<int32_t>(imm))));
                                    stage_store_slot(slot, MReg::RAX);
                                    continue;
                                }
                                /* @c addu/adds/subu/subs rN, src */
                                if ((opcode == "addu" || opcode == "adds"
                                  || opcode == "subu" || opcode == "subs") && args.size() == 2) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    if (dst_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(MReg::RAX, dst_slot);
                                    MOp aluop = (opcode[0] == 's') ? MOp::SUB : MOp::ADD;
                                    int64_t imm;
                                    int src_slot = vm_reg_slot_index(args[1]);
                                    if (src_slot >= 0) {
                                        stage_load_slot(MReg::RCX, src_slot);
                                        staged.push_back(MInstr::make_unary(aluop,
                                            MOperand::make_reg(MReg::RAX),
                                            MOperand::make_reg(MReg::RCX)));
                                    } else if (parse_imm_int(args[1], imm)
                                            && imm >= INT32_MIN && imm <= INT32_MAX) {
                                        staged.push_back(MInstr::make_unary(aluop,
                                            MOperand::make_reg(MReg::RAX),
                                            MOperand::make_imm32(static_cast<int32_t>(imm))));
                                    } else {
                                        all_ok = false; break;
                                    }
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    continue;
                                }
                                /* @c mov dst, src */
                                if (opcode == "mov" && args.size() == 2) {
                                    const std::string &dst = args[0];
                                    const std::string &src = args[1];

                                    /* Placeholders del frontend Vex: {dst} y {srcN}
                                     * referencian SSA values del IR instruction.
                                     * {dst}  -> slot SSA del ins.dst (en frame nativo)
                                     * {srcN} -> slot SSA de ins.operands[N]
                                     *
                                     * Patrones soportados aqui:
                                     *   mov {dst}, rN     -> store SSA[dst] = VMreg[N]
                                     *   mov {dst}, imm    -> store SSA[dst] = imm
                                     *   mov rN, {srcK}    -> store VMreg[N] = SSA[srcK]
                                     *   mov {dst}, {srcK} -> store SSA[dst] = SSA[srcK]
                                     */
                                    auto parse_placeholder = [&](const std::string &tok,
                                                                  int &out_src_idx,
                                                                  bool &is_dst) -> bool {
                                        out_src_idx = -1;
                                        is_dst = false;
                                        if (tok == "{dst}") { is_dst = true; return true; }
                                        if (tok.size() > 5
                                         && tok[0] == '{' && tok.back() == '}'
                                         && tok.compare(1, 3, "src") == 0) {
                                            int n = 0;
                                            for (size_t k = 4; k + 1 < tok.size(); ++k) {
                                                if (!std::isdigit(static_cast<unsigned char>(tok[k]))) return false;
                                                n = n * 10 + (tok[k] - '0');
                                            }
                                            out_src_idx = n;
                                            return true;
                                        }
                                        return false;
                                    };
                                    int  ph_src_idx_d = -1;  bool ph_is_dst_d = false;
                                    int  ph_src_idx_s = -1;  bool ph_is_dst_s = false;
                                    const bool dst_is_ph = parse_placeholder(dst, ph_src_idx_d, ph_is_dst_d);
                                    const bool src_is_ph = parse_placeholder(src, ph_src_idx_s, ph_is_dst_s);

                                    if (dst_is_ph || src_is_ph) {
                                        /* Helpers staged para SSA slots (en frame
                                         * nativo, accesibles via [rbp - 8*(vid+1)]). */
                                        auto stage_load_ssa = [&](MReg dst_reg, ir::IrValueId vid) {
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(dst_reg),
                                                slot_mem(vid)));
                                        };
                                        auto stage_store_ssa = [&](ir::IrValueId vid, MReg src_reg) {
                                            if (vid == ir::IR_NO_VALUE) return;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                slot_mem(vid),
                                                MOperand::make_reg(src_reg)));
                                        };

                                        /* Cargar valor a RAX. */
                                        if (src_is_ph) {
                                            if (ph_is_dst_s) {
                                                if (ins.dst == ir::IR_NO_VALUE) { all_ok = false; break; }
                                                stage_load_ssa(MReg::RAX, ins.dst);
                                            } else {
                                                if (ph_src_idx_s < 0
                                                 || static_cast<size_t>(ph_src_idx_s) >= ins.operands.size()) {
                                                    all_ok = false; break;
                                                }
                                                stage_load_ssa(MReg::RAX, ins.operands[ph_src_idx_s]);
                                            }
                                        } else {
                                            /* src es VMreg o imm. */
                                            int src_slot = vm_reg_slot_index(src);
                                            if (src_slot >= 0) {
                                                stage_load_slot(MReg::RAX, src_slot);
                                            } else {
                                                int64_t imm;
                                                if (!parse_imm_int(src, imm)) { all_ok = false; break; }
                                                stage_load_imm(MReg::RAX, imm);
                                            }
                                        }

                                        /* Guardar de RAX al destino. */
                                        if (dst_is_ph) {
                                            if (ph_is_dst_d) {
                                                if (ins.dst == ir::IR_NO_VALUE) { all_ok = false; break; }
                                                stage_store_ssa(ins.dst, MReg::RAX);
                                            } else {
                                                if (ph_src_idx_d < 0
                                                 || static_cast<size_t>(ph_src_idx_d) >= ins.operands.size()) {
                                                    all_ok = false; break;
                                                }
                                                stage_store_ssa(ins.operands[ph_src_idx_d], MReg::RAX);
                                            }
                                        } else {
                                            /* dst es VMreg. */
                                            int dst_slot = vm_reg_slot_index(dst);
                                            if (dst_slot < 0) { all_ok = false; break; }
                                            stage_store_slot(dst_slot, MReg::RAX);
                                        }
                                        continue;
                                    }

                                    /* resolver @Absolute("X") via callback.
                                     * `try_resolve_at` parsea `@Absolute("name")` o
                                     * `@StringRef("name")` y retorna la direccion
                                     * resuelta (con bandera de exito).  Si el callback
                                     * no esta o el simbolo no se encuentra, retorna
                                     * found=false. */
                                    auto try_resolve_at = [&](const std::string &tok,
                                                              int64_t &out_addr) -> bool {
                                        if (tok.size() < 2 || tok[0] != '@') return false;
                                        if (!opts_.resolve_symbol) return false;
                                        size_t lp = tok.find('(');
                                        size_t rp = tok.rfind(')');
                                        if (lp == std::string::npos || rp == std::string::npos
                                         || rp <= lp + 1) return false;
                                        std::string inner = tok.substr(lp + 1, rp - lp - 1);
                                        /* Quitar comillas si las hay. */
                                        if (inner.size() >= 2 && inner.front() == '"' && inner.back() == '"') {
                                            inner = inner.substr(1, inner.size() - 2);
                                        }
                                        if (inner.empty()) return false;
                                        const uint64_t v = opts_.resolve_symbol(inner);
                                        if (v == 0) return false;
                                        out_addr = static_cast<int64_t>(v);
                                        return true;
                                    };

                                    /* Si dst contiene @, el resolver podria devolver
                                     * la direccion donde escribir; pero esto solo
                                     * tendria sentido para usos atipicos.  Rechazamos. */
                                    if (dst.find('@') != std::string::npos) {
                                        all_ok = false; break;
                                    }
                                    const std::string dst_mem = parse_mem_operand(dst);
                                    const std::string src_mem = parse_mem_operand(src);
                                    int64_t resolved_addr = 0;
                                    const bool src_is_sym =
                                        src.find('@') != std::string::npos
                                     && try_resolve_at(src, resolved_addr);
                                    const bool src_is_unresolved_sym =
                                        src.find('@') != std::string::npos && !src_is_sym;
                                    if (src_is_unresolved_sym) {
                                        all_ok = false; break;
                                    }

                                    /* `mov rN, rM` / `mov rN, imm` / `mov rN, @Absolute(...)` */
                                    if (dst_mem.empty() && src_mem.empty()) {
                                        const int dst_slot = vm_reg_slot_index(dst);
                                        if (dst_slot < 0) { all_ok = false; break; }
                                        if (src_is_sym) {
                                            stage_load_imm(MReg::RAX, resolved_addr);
                                            stage_store_slot(dst_slot, MReg::RAX);
                                            continue;
                                        }
                                        int src_slot = vm_reg_slot_index(src);
                                        if (src_slot >= 0) {
                                            stage_load_slot(MReg::RAX, src_slot);
                                            stage_store_slot(dst_slot, MReg::RAX);
                                        } else {
                                            int64_t imm;
                                            if (!parse_imm_int(src, imm)) { all_ok = false; break; }
                                            stage_load_imm(MReg::RAX, imm);
                                            stage_store_slot(dst_slot, MReg::RAX);
                                        }
                                        continue;
                                    }
                                    /* `mov [rN], rM` / `mov [rN], imm` / `mov [rN], @Absolute(...)`:
                                     *    vrt_vm_write_u64(proc, vaddr, value) */
                                    if (!dst_mem.empty() && src_mem.empty()) {
                                        const int dst_slot = vm_reg_slot_index(dst_mem);
                                        if (dst_slot < 0) { all_ok = false; break; }
                                        if (src_is_sym) {
                                            stage_load_imm(ABI_ARG2, resolved_addr);
                                        } else {
                                            int src_slot = vm_reg_slot_index(src);
                                            if (src_slot >= 0) {
                                                stage_load_slot(ABI_ARG2, src_slot);
                                            } else {
                                                int64_t imm;
                                                if (!parse_imm_int(src, imm)) { all_ok = false; break; }
                                                stage_load_imm(ABI_ARG2, imm);
                                            }
                                        }
                                        stage_load_slot(ABI_ARG1, dst_slot);  /* vaddr */
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(ABI_ARG0),
                                            MOperand::make_reg(MReg::RBX)));
                                        stage_call(reinterpret_cast<uint64_t>(opts_.runtime->vm_write_u64));
                                        continue;
                                    }
                                    /* `mov rN, [rM]`:  vrt_vm_read_u64(proc, vaddr).
                                     *
                                     *INLINING.  Si la linea
                                     * ANTERIOR fue @c mov rM, @Absolute("X")
                                     * cuya resolucion dio vaddr V, y tenemos
                                     * @c read_vmem_u64, leemos el valor V[0..7]
                                     * en compile-time y emitimos un MOV
                                     * inmediato directo (1 instr, 0 calls)
                                     * en lugar de la llamada a vm_read_u64.
                                     *
                                     * Esto elimina 1 runtime call por
                                     * @c new ClassName() (la lectura del slot
                                     * @c s_X que cachea @c ClassInfo*).  En
                                     * benches @c 100_reflection_full con 30M
                                     * iteraciones es uno de los hot calls. */
                                    if (dst_mem.empty() && !src_mem.empty()) {
                                        const int dst_slot = vm_reg_slot_index(dst);
                                        const int src_slot = vm_reg_slot_index(src_mem);
                                        if (dst_slot < 0 || src_slot < 0) { all_ok = false; break; }
                                        /*si la linea ANTERIOR
                                         * fue @c mov rM, @Absolute("X") con
                                         * X resoluble a vaddr V, podemos:
                                         *   (a) inlinear el valor si ya esta
                                         *       cacheado (read_vmem_u64 != 0).
                                         *   (b) si NO, emitir un check-cache
                                         *       inline con IC slot.
                                         *
                                         * Hot path con IC slot (3 instrs +
                                         * branch predicted): @c mov rN,
                                         * [ic_slot]; @c test rN, rN; @c jne
                                         * done -> 3-5 ns vs 30-50 ns del
                                         * call a @c vrt_vm_read_u64. */
                                        bool inlined = false;
                                        if (dst_slot == src_slot
                                         && li > 0) {
                                            const std::string &prev_line = lines[li - 1];
                                            size_t sp2 = prev_line.find_first_of(" \t");
                                            std::string prev_op = (sp2 == std::string::npos)
                                                ? prev_line : prev_line.substr(0, sp2);
                                            if (prev_op == "mov") {
                                                std::string prev_rest = trim_str(
                                                    prev_line.substr(sp2));
                                                auto prev_args = split_csv(prev_rest);
                                                if (prev_args.size() == 2
                                                 && prev_args[0] == src_mem
                                                 && prev_args[1].find('@') != std::string::npos) {
                                                    int64_t v_addr = 0;
                                                    if (try_resolve_at(prev_args[1], v_addr)) {
                                                        uint64_t cached_value = 0;
                                                        if (opts_.read_vmem_u64) {
                                                            cached_value = opts_.read_vmem_u64(
                                                                static_cast<uint64_t>(v_addr));
                                                        }
                                                        if (cached_value != 0) {
                                                            /* Inline directo con imm64. */
                                                            stage_load_imm(MReg::RAX,
                                                                static_cast<int64_t>(cached_value));
                                                            stage_store_slot(dst_slot, MReg::RAX);
                                                            inlined = true;
                                                        } else if (opts_.reserve_ic_slot) {
                                                            /* Inline cache: reservar 8 bytes en
                                                             * code cache, init a 0.  Primera vez
                                                             * popula via vm_read_u64; despues hot path. */
                                                            const uint64_t ic = opts_.reserve_ic_slot();
                                                            if (ic) {
                                                                /* mov r10, ic_slot_addr */
                                                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                                                    MOperand::make_reg(MReg::R10),
                                                                    MOperand::make_imm64_idx(
                                                                        mf.intern_imm64(ic))));
                                                                /* mov rax, [r10]  (load cached val) */
                                                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                                                    MOperand::make_reg(MReg::RAX),
                                                                    MOperand::make_mem(MReg::R10, 0)));
                                                                /* test rax, rax */
                                                                MInstr ti;
                                                                ti.op = MOp::TEST;
                                                                ti.dst = MOperand::make_reg(MReg::RAX);
                                                                ti.src1 = MOperand::make_reg(MReg::RAX);
                                                                staged.push_back(ti);
                                                                /* jne done */
                                                                const MLabelId done_lbl = mf.new_label();
                                                                staged.push_back(
                                                                    MInstr::make_jcc(MCond::NE, done_lbl));
                                                                /* Cold: vm_read_u64 + store IC. */
                                                                stage_load_imm(ABI_ARG1,
                                                                    static_cast<int64_t>(v_addr));
                                                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                                                    MOperand::make_reg(ABI_ARG0),
                                                                    MOperand::make_reg(MReg::RBX)));
                                                                stage_call(reinterpret_cast<uint64_t>(
                                                                    opts_.runtime->vm_read_u64));
                                                                /* mov [r10], rax  (cache result) */
                                                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                                                    MOperand::make_reg(MReg::R10),
                                                                    MOperand::make_imm64_idx(
                                                                        mf.intern_imm64(ic))));
                                                                staged.push_back(MInstr::make_unary(MOp::MOV,
                                                                    MOperand::make_mem(MReg::R10, 0),
                                                                    MOperand::make_reg(MReg::RAX)));
                                                                /* done: */
                                                                staged.push_back(
                                                                    MInstr::make_label_def(done_lbl));
                                                                /* Result in rax -> store al slot. */
                                                                stage_store_slot(dst_slot, MReg::RAX);
                                                                inlined = true;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        if (!inlined) {
                                            stage_load_slot(ABI_ARG1, src_slot);  /* vaddr */
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(ABI_ARG0),
                                                MOperand::make_reg(MReg::RBX)));
                                            stage_call(reinterpret_cast<uint64_t>(opts_.runtime->vm_read_u64));
                                            stage_store_slot(dst_slot, MReg::RAX);
                                        }
                                        continue;
                                    }
                                    all_ok = false; break;
                                }
                                /* @c findclass rN, rM:  vrt_findclass(proc, vaddr_from_rM) -> rN */
                                if (opcode == "findclass" && args.size() == 2
                                 && opts_.runtime->findclass) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    const int src_slot = vm_reg_slot_index(args[1]);
                                    if (dst_slot < 0 || src_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG1, src_slot);  /* params_vaddr */
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->findclass));
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    continue;
                                }
                                /* @c newobj rN: convencion bytecode -> r0 = HANDLE.
                                 *   1. host_ptr = vrt_newobj(proc, cls_in_rN)
                                 *   2. handle   = vrt_gc_handle_for_ptr(proc, host_ptr)
                                 *   3. r0       = handle
                                 * El frontend siempre hace `gcderef cur0, r0; xchg cur0, rM`
                                 * justo despues, lo que re-deriva host_ptr de forma correcta.
                                 *
                                 * si las DOS lineas siguientes son
                                 * exactamente @c gcderef cur0, r0 + @c xchg cur0, rM,
                                 * fusionamos: llamamos directamente @c vrt_newobj
                                 * (que retorna host_ptr) y guardamos en slot(rM)
                                 * + slot(r0).  Salta 1 runtime call (vrt_gc_deref). */
                                if (opcode == "newobj" && args.size() == 1
                                 && opts_.runtime->newobj_handle) {
                                    const int slot = vm_reg_slot_index(args[0]);
                                    if (slot < 0) { all_ok = false; break; }
                                    /* Look-ahead: gcderef + xchg? */
                                    bool fused = false;
                                    if (li + 2 < lines.size()
                                     && opts_.runtime->newobj) {
                                        const std::string &l1 = lines[li + 1];
                                        const std::string &l2 = lines[li + 2];
                                        /* l1: "gcderef cur0, r0" */
                                        /* l2: "xchg cur0, rM" */
                                        size_t s1 = l1.find_first_of(" \t");
                                        size_t s2 = l2.find_first_of(" \t");
                                        std::string op1 = (s1 == std::string::npos) ? l1 : l1.substr(0, s1);
                                        std::string op2 = (s2 == std::string::npos) ? l2 : l2.substr(0, s2);
                                        if (op1 == "gcderef" && op2 == "xchg") {
                                            std::string r1 = (s1 == std::string::npos) ? "" : trim_str(l1.substr(s1));
                                            std::string r2 = (s2 == std::string::npos) ? "" : trim_str(l2.substr(s2));
                                            auto a1 = split_csv(r1);
                                            auto a2 = split_csv(r2);
                                            if (a1.size() == 2 && a1[0] == "cur0" && a1[1] == "r0"
                                             && a2.size() == 2 && a2[0] == "cur0") {
                                                const int dest_slot = vm_reg_slot_index(a2[1]);
                                                if (dest_slot >= 0) {
                                                    /* Emitir: vrt_newobj(proc, cls) -> host_ptr en rax,
                                                     * y storear a slot(rM) y slot(r0). */
                                                    stage_load_slot(ABI_ARG1, slot);
                                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                                        MOperand::make_reg(ABI_ARG0),
                                                        MOperand::make_reg(MReg::RBX)));
                                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->newobj));
                                                    stage_store_slot(dest_slot, MReg::RAX);
                                                    stage_store_slot(0, MReg::RAX);  /* r0 = host_ptr tambien */
                                                    li += 2;  /* skipear gcderef + xchg */
                                                    fused = true;
                                                }
                                            }
                                        }
                                    }
                                    if (fused) continue;
                                    /* Fallback: ruta original via newobj_handle + handle. */
                                    stage_load_slot(ABI_ARG1, slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->newobj_handle));
                                    /* r0 = handle. */
                                    stage_store_slot(0, MReg::RAX);
                                    continue;
                                }
                                /* @c defclass rN, rM */
                                if (opcode == "defclass" && args.size() == 2
                                 && opts_.runtime->defclass) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    const int src_slot = vm_reg_slot_index(args[1]);
                                    if (dst_slot < 0 || src_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG1, src_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->defclass));
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    continue;
                                }
                                /* @c deffield rN, rM: vrt_deffield(proc, rN, rM_vaddr) -> r0 */
                                if (opcode == "deffield" && args.size() == 2
                                 && opts_.runtime->deffield) {
                                    const int cls_slot = vm_reg_slot_index(args[0]);
                                    const int prm_slot = vm_reg_slot_index(args[1]);
                                    if (cls_slot < 0 || prm_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG2, prm_slot);
                                    stage_load_slot(ABI_ARG1, cls_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->deffield));
                                    stage_store_slot(0, MReg::RAX);  /* r0 = 1/0 */
                                    continue;
                                }
                                /* @c defmethod rN, rM: vtable_idx en r0. */
                                if (opcode == "defmethod" && args.size() == 2
                                 && opts_.runtime->defmethod) {
                                    const int cls_slot = vm_reg_slot_index(args[0]);
                                    const int prm_slot = vm_reg_slot_index(args[1]);
                                    if (cls_slot < 0 || prm_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG2, prm_slot);
                                    stage_load_slot(ABI_ARG1, cls_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->defmethod));
                                    stage_store_slot(0, MReg::RAX);
                                    continue;
                                }
                                /* @c findmethod / @c findfield rN, rM (o {dst}, {srcN}) */
                                if (opcode == "findmethod" && args.size() == 2
                                 && opts_.runtime->findmethod) {
                                    /* Helper: cargar argN como ABI_ARG1 desde
                                     * VMreg, SSA value placeholder ({dst}/{srcK}), o imm. */
                                    auto stage_load_arg_to = [&](MReg dst_reg, const std::string &tok) -> bool {
                                        if (tok == "{dst}") {
                                            if (ins.dst == ir::IR_NO_VALUE) return false;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(dst_reg), slot_mem(ins.dst)));
                                            return true;
                                        }
                                        if (tok.size() > 5 && tok[0] == '{'
                                         && tok.back() == '}' && tok.compare(1, 3, "src") == 0) {
                                            int n = 0;
                                            for (size_t k = 4; k + 1 < tok.size(); ++k) {
                                                if (!std::isdigit(static_cast<unsigned char>(tok[k]))) return false;
                                                n = n * 10 + (tok[k] - '0');
                                            }
                                            if (n < 0 || static_cast<size_t>(n) >= ins.operands.size()) return false;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(dst_reg), slot_mem(ins.operands[n])));
                                            return true;
                                        }
                                        int s = vm_reg_slot_index(tok);
                                        if (s >= 0) { stage_load_slot(dst_reg, s); return true; }
                                        return false;
                                    };
                                    auto stage_store_arg_from = [&](const std::string &tok, MReg src_reg) -> bool {
                                        if (tok == "{dst}") {
                                            if (ins.dst == ir::IR_NO_VALUE) return false;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                slot_mem(ins.dst), MOperand::make_reg(src_reg)));
                                            return true;
                                        }
                                        if (tok.size() > 5 && tok[0] == '{'
                                         && tok.back() == '}' && tok.compare(1, 3, "src") == 0) {
                                            int n = 0;
                                            for (size_t k = 4; k + 1 < tok.size(); ++k) {
                                                if (!std::isdigit(static_cast<unsigned char>(tok[k]))) return false;
                                                n = n * 10 + (tok[k] - '0');
                                            }
                                            if (n < 0 || static_cast<size_t>(n) >= ins.operands.size()) return false;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                slot_mem(ins.operands[n]), MOperand::make_reg(src_reg)));
                                            return true;
                                        }
                                        int s = vm_reg_slot_index(tok);
                                        if (s >= 0) { stage_store_slot(s, src_reg); return true; }
                                        return false;
                                    };

                                    if (!stage_load_arg_to(ABI_ARG1, args[1])) { all_ok = false; break; }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->findmethod));
                                    if (!stage_store_arg_from(args[0], MReg::RAX)) { all_ok = false; break; }
                                    continue;
                                }
                                if (opcode == "findfield" && args.size() == 2
                                 && opts_.runtime->findfield) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    const int prm_slot = vm_reg_slot_index(args[1]);
                                    if (dst_slot < 0 || prm_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG1, prm_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->findfield));
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    continue;
                                }
                                /* @c push rN -> subsp rsp,8 + write_u64(rsp, rN). */
                                if (opcode == "push" && args.size() == 1
                                 && opts_.runtime->vm_write_u64) {
                                    const int src_slot = vm_reg_slot_index(args[0]);
                                    if (src_slot < 0) { all_ok = false; break; }
                                    /* rsp -= 8 */
                                    stage_load_slot(MReg::RAX, 16 /*rsp*/);
                                    staged.push_back(MInstr::make_unary(MOp::SUB,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_imm32(8)));
                                    stage_store_slot(16, MReg::RAX);
                                    /* vrt_vm_write_u64(proc, rsp_new, rN) */
                                    stage_load_slot(ABI_ARG2, src_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG1),
                                        MOperand::make_reg(MReg::RAX)));
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->vm_write_u64));
                                    continue;
                                }
                                /* @c pop rN -> read_u64(rsp) + rsp += 8. */
                                if (opcode == "pop" && args.size() == 1
                                 && opts_.runtime->vm_read_u64) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    if (dst_slot < 0) { all_ok = false; break; }
                                    /* read value desde [rsp] */
                                    stage_load_slot(ABI_ARG1, 16 /*rsp*/);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->vm_read_u64));
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    /* rsp += 8 */
                                    stage_load_slot(MReg::RAX, 16);
                                    staged.push_back(MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_imm32(8)));
                                    stage_store_slot(16, MReg::RAX);
                                    continue;
                                }
                                /* @c gchandle rN, rM -> vrt_gc_handle_for_ptr. */
                                if (opcode == "gchandle" && args.size() == 2
                                 && opts_.runtime->gc_handle_for_ptr) {
                                    const int dst_slot = vm_reg_slot_index(args[0]);
                                    const int src_slot = vm_reg_slot_index(args[1]);
                                    if (dst_slot < 0 || src_slot < 0) { all_ok = false; break; }
                                    stage_load_slot(ABI_ARG1, src_slot);  /* host_ptr */
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->gc_handle_for_ptr));
                                    stage_store_slot(dst_slot, MReg::RAX);
                                    continue;
                                }
                                /* @c gcderef cur0, rN + @c xchg cur0, rM
                                 * patron par inseparable -> rM = vrt_gc_deref(rN). */
                                if (opcode == "gcderef" && args.size() == 2
                                 && args[0] == "cur0"
                                 && opts_.runtime->gc_deref) {
                                    const int handle_slot = vm_reg_slot_index(args[1]);
                                    if (handle_slot < 0) { all_ok = false; break; }
                                    /* Peek next: must be `xchg cur0, rM`. */
                                    if (li + 1 >= lines.size()) { all_ok = false; break; }
                                    const std::string &nxt = lines[li + 1];
                                    size_t sp2 = nxt.find_first_of(" \t");
                                    std::string op2 = (sp2 == std::string::npos)
                                                          ? nxt : nxt.substr(0, sp2);
                                    std::string rest2 = (sp2 == std::string::npos)
                                                          ? "" : trim_str(nxt.substr(sp2));
                                    auto args2 = split_csv(rest2);
                                    if (op2 != "xchg" || args2.size() != 2 || args2[0] != "cur0") {
                                        all_ok = false; break;
                                    }
                                    const int out_slot = vm_reg_slot_index(args2[1]);
                                    if (out_slot < 0) { all_ok = false; break; }
                                    /* vrt_gc_deref(proc, handle) -> host_ptr */
                                    stage_load_slot(ABI_ARG1, handle_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->gc_deref));
                                    stage_store_slot(out_slot, MReg::RAX);
                                    ++li;  /* consumimos la linea xchg tambien */
                                    continue;
                                }
                                /* @c callvirt rN, idx -> vrt_callvirt(proc, [rN], idx). */
                                if (opcode == "callvirt" && args.size() == 2
                                 && opts_.runtime->callvirt) {
                                    const int obj_slot = vm_reg_slot_index(args[0]);
                                    int64_t idx;
                                    if (obj_slot < 0 || !parse_imm_int(args[1], idx)
                                     || idx < 0 || idx > INT32_MAX) {
                                        all_ok = false; break;
                                    }
                                    stage_load_slot(ABI_ARG1, obj_slot);  /* obj_payload */
                                    stage_load_imm(ABI_ARG2, idx);         /* vtbl_idx */
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->callvirt));
                                    /* result en RAX, pero vrt_callvirt ya escribe
                                     * el return a proc->registers.regs[0] via su
                                     * propio mecanismo de dispatch.  No-op aqui. */
                                    continue;
                                }
                                /* @c setmethdbg rN, rM ->
                                 * NO-OP en JIT.  Debug info (file:line per
                                 * method, usado por build_stack_trace para
                                 * stack traces enriquecidos) NO es critico
                                 * para la ejecucion del programa.  Saltarlo
                                 * en JIT permite que `__module_init` se
                                 * eager-compile sin riesgo de crash (la
                                 * llamada a register_method_debug tras un
                                 * potencial JIT context puede toparse con
                                 * estado runtime no inicializado).  El
                                 * bytecode interp del programa, si se
                                 * ejecuta primero, ya registro la debug
                                 * info; si solo JIT corre, los stack traces
                                 * pierden file:line pero ejecutan bien. */
                                if (opcode == "setmethdbg" && args.size() == 2) {
                                    /* Emitir 0 MInstrs: no-op compilable. */
                                    continue;
                                }
                                /* @c addadvice rN, rM, kind */
                                if (opcode == "addadvice" && args.size() == 3
                                 && opts_.runtime->addadvice) {
                                    const int tgt_slot = vm_reg_slot_index(args[0]);
                                    const int adv_slot = vm_reg_slot_index(args[1]);
                                    int64_t kind;
                                    if (tgt_slot < 0 || adv_slot < 0
                                     || !parse_imm_int(args[2], kind)) {
                                        all_ok = false; break;
                                    }
                                    stage_load_imm(ABI_ARG3, kind);
                                    stage_load_slot(ABI_ARG2, adv_slot);
                                    stage_load_slot(ABI_ARG1, tgt_slot);
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(opts_.runtime->addadvice));
                                    stage_store_slot(0, MReg::RAX);
                                    continue;
                                }
                                /* Opcode no reconocido por el mini-parser. */
                                all_ok = false;
                                break;
                            }

                            if (all_ok && !staged.empty()) {
                                /* Commit: emitir un stackmap conjunto para
                                 * todas las MInstrs (cubre cualquier CALL
                                 * embebido como un solo safepoint). */
                                for (auto &mi : staged) {
                                    /* Si la instr es un CALL, emitir su
                                     * stackmap antes de commit. */
                                    if (mi.op == MOp::CALL) {
                                        emit_stackmap_for_safepoint(mi);
                                    }
                                    mf.blocks.back().instrs.push_back(std::move(mi));
                                }
                                matched = true;
                            }
                        }

                        if (!matched) {
                            /* Patron raw_asm no reconocido.  Warning + unsupported. */
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(
                                    static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    /* Truncar el texto del asm a 60 chars
                                     * para mensaje legible (puede tener \n). */
                                    std::string preview = asm_text;
                                    for (char &c : preview) if (c == '\n') c = ' ';
                                    if (preview.size() > 60) preview.resize(60);
                                    std::fprintf(stderr,
                                        "[jit] selector: raw_asm '%s...' no reconocido en fn '%s' linea %u\n",
                                        preview.c_str(),
                                        ir_fn.name.c_str(),
                                        ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                        }
                        break;
                    }

                    default:
                        /* Op no soportada en v1.  Marcar como unsupported
                         * y emitir INT3 para que la ejecucion crashee
                         * controladamente.
                         *
                         * Si g_jit_warn_unsupported esta activo (env var
                         * VESTA_JIT_WARN_UNSUPPORTED=1), imprimir nombre de
                         * la op faltante + funcion + linea fuente -- una sola
                         * vez por (op, line) gracias al dedup en warn_unsupported. */
                        warn_unsupported(ins.op, ins.source_line, nullptr);
                        unsupported = true;
                        mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                        break;
                }
                /* D.2-int: tras procesar la instruccion, si su dst es GC
                 * marcamos el slot en live_gc_slots.  Asi cualquier
                 * SAFEPOINT posterior incluira este slot en su stackmap. */
                if (ins.dst != ir::IR_NO_VALUE) {
                    mark_slot_as_gc_if_needed(ins.dst, ins.type);
                }
            }

            /* Si el IrBlock no termina con RET/BR/BR_COND explicito y
             * tiene un sucesor unico, agregar fallthrough JMP.  En
             * general el IR Vex ya garantiza terminator, pero el v1
             * es defensivo. */
            if (!ir_block.instrs.empty()) {
                const auto last_op = ir_block.instrs.back().op;
                if (last_op != ir::IrOp::RET
                 && last_op != ir::IrOp::BR
                 && last_op != ir::IrOp::BR_COND) {
                    if (ir_block.succs.size() == 1
                     && ir_block.succs[0] < block_labels.size()) {
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jmp(block_labels[ir_block.succs[0]]));
                    }
                }
            }
            (void)mb;
        }

        /* Peep-hole post-emision (conservativo).
         *
         * Nota historica: iterar peephole+DSE 2 veces produjo codigo
         * mas pequeno (~7%) pero ralentizo el wall time medido (~20%
         * peor: 30ms -> 36ms en bench_jit_method) probablemente por
         * efectos de alignment/cache.  Mantenemos 1 sola iteracion. */
        {
        /* Peep-hole post-emision (conservativo):
         *
         * Pattern 1: mov reg, reg con mismo reg -> eliminar (e.g. mov rax,rax).
         *
         * Pattern 2: LOAD redundante del mismo slot al mismo reg.
         *   mov rax, [rbp-8]
         *   ...                       (instrs que no clobean rax ni escriben [rbp-8])
         *   mov rax, [rbp-8]   <-     ELIMINAR
         *
         * Pattern 3: LOAD a un reg distinto del slot ya cargado en otro reg.
         *   mov rax, [rbp-8]   ; rax ahora tiene [rbp-8]
         *   mov rcx, [rbp-8]   <-     CONVERTIR a "mov rcx, rax" (1 byte menos
         *                             y elimina memory access en hot path)
         *
         * Pattern 4: STORE seguido del MISMO LOAD inmediato sin clobber.
         *   mov [rbp-8], rax
         *   mov rax, [rbp-8]   <-     ELIMINAR (rax ya tiene el valor)
         *
         * Limitaciones (conservadoras para evitar incorrectness):
         *   - Solo dentro del MISMO block.
         *   - CALL invalida TODOS los regs (caller-saved + safety).
         *   - STORE invalida cualquier reg cuyo mem_slot coincida con dst.
         *   - Cualquier ALU op invalida su dst reg.
         */
        /* Estado tracking compartido entre bloques (cross-block dataflow). */
        struct RegState {
            bool         valid     = false;
            uint8_t      mem_base  = 255;
            int32_t      mem_disp  = INT32_MIN;
            uint8_t      mem_width = 255;
            bool         has_imm32 = false;
            int32_t      imm32_val = 0;
        };

        /* Predecesores de cada bloque (para entry_state inheritance).
         * El selector NO setea succ_a/succ_b explicitamente; los derivamos
         * escaneando las instrucciones JMP/JCC dentro de cada bloque. */
        const size_t N_BLOCKS = mf.blocks.size();
        std::unordered_map<uint32_t, size_t> label_to_block;
        for (size_t i = 0; i < N_BLOCKS; ++i) {
            if (mf.blocks[i].label_id != MLABEL_INVALID) {
                label_to_block[mf.blocks[i].label_id] = i;
            }
        }
        std::vector<std::vector<size_t>> preds(N_BLOCKS);
        for (size_t i = 0; i < N_BLOCKS; ++i) {
            const auto &binstrs = mf.blocks[i].instrs;
            bool has_uncond_jmp = false;
            for (const auto &mi : binstrs) {
                if ((mi.op == MOp::JMP || mi.op == MOp::JCC)
                 && mi.src1.kind == MOperandKind::LABEL) {
                    auto it = label_to_block.find(
                        static_cast<uint32_t>(mi.src1.value));
                    if (it != label_to_block.end()) {
                        preds[it->second].push_back(i);
                    }
                    if (mi.op == MOp::JMP) has_uncond_jmp = true;
                }
            }
            /* Fall-through al siguiente bloque si NO hay JMP unconditional
             * y este no es el ultimo bloque + tampoco termina en RET. */
            if (!has_uncond_jmp && (i + 1) < N_BLOCKS) {
                bool ends_in_ret = false;
                for (const auto &mi : binstrs) {
                    if (mi.op == MOp::RET) { ends_in_ret = true; break; }
                }
                if (!ends_in_ret) {
                    preds[i + 1].push_back(i);
                }
            }
        }
        /* exit_states[i] = state del reg_has al final del bloque i. */
        std::vector<std::array<RegState, 64>> exit_states(N_BLOCKS);

        for (size_t bi = 0; bi < N_BLOCKS; ++bi) {
            auto &block = mf.blocks[bi];
            RegState reg_has[64];
            /* Heredar entry_state: si exactamente 1 pred Y ese pred es
             * ANTERIOR en orden (i.e. ya procesado), copiamos su exit_state.
             * Si multiple preds, hacemos interseccion conservativa.  Si el
             * unico pred es POSTERIOR (back-edge en loop), no heredamos. */
            if (preds[bi].size() == 1 && preds[bi][0] < bi) {
                for (int r = 0; r < 64; ++r) {
                    reg_has[r] = exit_states[preds[bi][0]][r];
                }
            } else if (preds[bi].size() > 1) {
                /* Interseccion: solo mantenemos un slot si TODOS los preds
                 * (que ya fueron procesados) coinciden en el mismo (base,
                 * disp, width).  Si algun pred es posterior (loop), no
                 * heredamos esos slots (conservativo). */
                bool first = true;
                for (size_t p : preds[bi]) {
                    if (p >= bi) {
                        /* Pred no procesado aun (back-edge): invalidar todo. */
                        for (int r = 0; r < 64; ++r) reg_has[r] = {};
                        first = false;
                        break;
                    }
                    if (first) {
                        for (int r = 0; r < 64; ++r) {
                            reg_has[r] = exit_states[p][r];
                        }
                        first = false;
                    } else {
                        for (int r = 0; r < 64; ++r) {
                            if (reg_has[r].valid &&
                                (reg_has[r].mem_base != exit_states[p][r].mem_base ||
                                 reg_has[r].mem_disp != exit_states[p][r].mem_disp ||
                                 reg_has[r].mem_width != exit_states[p][r].mem_width ||
                                 !exit_states[p][r].valid)) {
                                reg_has[r].valid = false;
                            }
                            if (reg_has[r].has_imm32 &&
                                (!exit_states[p][r].has_imm32 ||
                                 reg_has[r].imm32_val != exit_states[p][r].imm32_val)) {
                                reg_has[r].has_imm32 = false;
                            }
                        }
                    }
                }
            }
            /* Tracking del ultimo SHL para detectar SHL+SAR pair. */
            int last_shl_reg = -1;
            int last_shl_count = -1;

            /* Solo trackeamos slots SSA puros (base = RBP, sin index reg).
             * Cualquier otra forma de memoria (e.g. [rcx], [rbp+rdx*8], etc.)
             * puede tener aliasing con stores arbitrarios -> no safe. */
            const uint8_t RBP_REG = reg_id(MReg::RBP);
            auto is_slot_mem = [&](const MOperand &op) -> bool {
                if (op.kind != MOperandKind::MEM) return false;
                if (op.reg != RBP_REG) return false;
                /* width packed: bits[1:0] scale, bits[7:2] index_reg.
                 * Slot puro: index_reg=63 (none), scale=0. */
                if ((op.width & 0xFC) != 0xFC) return false;
                return true;
            };

            auto same_mem = [](const MOperand &op, uint8_t base,
                               int32_t disp, uint8_t width) -> bool {
                return op.kind == MOperandKind::MEM
                    && op.reg   == base
                    && op.value == disp
                    && op.width == width;
            };
            auto invalidate_reg = [&](uint8_t r) {
                if (r < 64) reg_has[r] = {};
            };
            auto invalidate_all = [&]() {
                for (auto &r : reg_has) r = {};
            };
            /* Invalidar TODOS los regs que crean tener un slot que coincide
             * con el slot recien escrito (porque ya no tienen ese valor). */
            auto invalidate_slot_holders = [&](const MOperand &mem_op) {
                if (mem_op.kind != MOperandKind::MEM) return;
                for (auto &r : reg_has) {
                    if (r.valid && r.mem_base == mem_op.reg
                     && r.mem_disp == mem_op.value
                     && r.mem_width == mem_op.width) {
                        r = {};
                    }
                }
            };

            std::vector<MInstr> out;
            out.reserve(block.instrs.size());

            for (const auto &mi : block.instrs) {
                MInstr emit_mi = mi;
                bool skip = false;

                /* Pattern 1: mov reg, reg con mismo reg. */
                if (emit_mi.op == MOp::MOV
                 && emit_mi.dst.kind == MOperandKind::REG
                 && emit_mi.src1.kind == MOperandKind::REG
                 && emit_mi.dst.reg == emit_mi.src1.reg) {
                    skip = true;
                }

                /* Pattern 2/3/4: MOV reg, [mem].  SOLO si es slot SSA puro. */
                if (!skip && emit_mi.op == MOp::MOV
                 && emit_mi.dst.kind == MOperandKind::REG
                 && emit_mi.src1.kind == MOperandKind::MEM
                 && emit_mi.dst.reg < 64
                 && is_slot_mem(emit_mi.src1)) {
                    const uint8_t dst_r   = emit_mi.dst.reg;
                    const uint8_t mem_b   = emit_mi.src1.reg;
                    const int32_t mem_d   = emit_mi.src1.value;
                    const uint8_t mem_w   = emit_mi.src1.width;
                    /* Pattern 2: dst_r ya tiene este slot? -> skip. */
                    if (reg_has[dst_r].valid
                     && reg_has[dst_r].mem_base == mem_b
                     && reg_has[dst_r].mem_disp == mem_d
                     && reg_has[dst_r].mem_width == mem_w) {
                        skip = true;
                    } else {
                        /* Pattern 3: algun OTRO reg ya tiene este slot? ->
                         * convertir LOAD a mov reg, otro_reg.
                         *
                         * EXCEPCION (Phase D.7): si el slot corresponde a
                         * un VID pinned por regalloc, NO convertir.  La
                         * razon: tras el rewrite el slot se convierte en
                         * un acceso a reg, asi que la "memory access
                         * saving" de Pattern 3 es moot.  Peor: la
                         * propagacion via otro reg ROMPE el destructive
                         * in-place peephole post-rewrite (el m0 quedaria
                         * con src=rcx en vez de src=r12 directo). */
                        bool slot_pinned_by_regalloc = false;
                        if (!regalloc.empty()
                         && mem_b == reg_id(MReg::RBP)) {
                            const int32_t vid = slot_offset_to_vid(mem_d);
                            if (vid >= 0
                             && regalloc.vid_to_reg.count(static_cast<ir::IrValueId>(vid))) {
                                slot_pinned_by_regalloc = true;
                            }
                        }
                        if (!slot_pinned_by_regalloc) {
                            int src_reg_with_slot = -1;
                            for (int r = 0; r < 64; ++r) {
                                if (reg_has[r].valid
                                 && reg_has[r].mem_base == mem_b
                                 && reg_has[r].mem_disp == mem_d
                                 && reg_has[r].mem_width == mem_w) {
                                    src_reg_with_slot = r;
                                    break;
                                }
                            }
                            if (src_reg_with_slot >= 0
                             && src_reg_with_slot != dst_r) {
                                /* Reemplazar src1 con un REG operand. */
                                emit_mi.src1 = MOperand::make_reg(
                                    static_cast<MReg>(src_reg_with_slot));
                            }
                        }
                    }
                }

                /* Pattern 5: SHL r, imm32; SAR r, imm32 con misma shift count
                 * sobre un reg que viene de "mov r, imm32" sign-ext.
                 * El sign-extend a partir de imm32 ya es canonico, por tanto
                 * SHL+SAR son no-op cuando shift = 64 - 32 = 32.  Tambien
                 * eliminamos cuando reg tiene "valor i32 valido" (limit comun).
                 *
                 * Tambien folding general: tras un STORE i32 (RMW eliminado en
                 * favor de mov dword nativo), el reg del src tiene los bits
                 * altos = sign-ext del valor de 32.  SHL+SAR es no-op tambien
                 * cuando reg ya tiene un i32 sign-extended cargado via
                 * mov32 (que x86-64 zero-extend automaticamente).
                 *
                 * Implementacion: si SHL count == SAR/SHR count siguiente, y el
                 * reg origen es el mismo, asumimos canonico y skipeamos AMBOS. */
                if (!skip
                 && emit_mi.dst.kind == MOperandKind::REG
                 && emit_mi.src1.kind == MOperandKind::IMM32
                 && emit_mi.dst.reg < 64) {
                    const uint8_t r = emit_mi.dst.reg;
                    if (emit_mi.op == MOp::SHL) {
                        /* Si shift count es 32 (= 64-32), y el reg viene de
                         * un imm32 conocido (que cabe en i32), los upper bits
                         * son sign-ext de ese imm.  SHL count + (proximo SAR
                         * count) es no-op. */
                        if (emit_mi.src1.value == 32
                         && reg_has[r].has_imm32) {
                            last_shl_reg = r;
                            last_shl_count = emit_mi.src1.value;
                            skip = true;
                        }
                    } else if (emit_mi.op == MOp::SAR
                            || emit_mi.op == MOp::SHR) {
                        if (last_shl_reg == r
                         && last_shl_count == emit_mi.src1.value) {
                            skip = true;
                            last_shl_reg = -1;
                        }
                    }
                }
                if (!skip && emit_mi.op != MOp::SHL
                 && emit_mi.op != MOp::SAR && emit_mi.op != MOp::SHR) {
                    last_shl_reg = -1;
                }

                if (skip) continue;

                /* Actualizar tracking POST-emit. */
                if (emit_mi.op == MOp::MOV) {
                    if (emit_mi.dst.kind == MOperandKind::REG
                     && emit_mi.src1.kind == MOperandKind::MEM
                     && emit_mi.dst.reg < 64) {
                        /* LOAD desde slot SSA: tracking; LOAD desde otro mem:
                         * invalidar dst reg (puede aliasing). */
                        if (is_slot_mem(emit_mi.src1)) {
                            reg_has[emit_mi.dst.reg] = {
                                true, emit_mi.src1.reg,
                                emit_mi.src1.value, emit_mi.src1.width};
                        } else {
                            invalidate_reg(emit_mi.dst.reg);
                        }
                    } else if (emit_mi.dst.kind == MOperandKind::REG
                            && emit_mi.src1.kind == MOperandKind::REG
                            && emit_mi.dst.reg < 64
                            && emit_mi.src1.reg < 64) {
                        reg_has[emit_mi.dst.reg] = reg_has[emit_mi.src1.reg];
                    } else if (emit_mi.dst.kind == MOperandKind::MEM
                            && emit_mi.src1.kind == MOperandKind::REG
                            && emit_mi.src1.reg < 64) {
                        if (is_slot_mem(emit_mi.dst)) {
                            invalidate_slot_holders(emit_mi.dst);
                            reg_has[emit_mi.src1.reg] = {
                                true, emit_mi.dst.reg,
                                emit_mi.dst.value, emit_mi.dst.width};
                        } else {
                            /* STORE a memoria arbitraria: cualquier slot
                             * podria aliasing.  Invalidar TODOS los regs. */
                            invalidate_all();
                        }
                    } else if (emit_mi.dst.kind == MOperandKind::MEM) {
                        if (is_slot_mem(emit_mi.dst)) {
                            invalidate_slot_holders(emit_mi.dst);
                        } else {
                            invalidate_all();  /* memoria arbitraria */
                        }
                    } else if (emit_mi.dst.kind == MOperandKind::REG
                            && emit_mi.src1.kind == MOperandKind::IMM32
                            && emit_mi.dst.reg < 64) {
                        /* mov reg, imm32: trackear el valor para folding. */
                        reg_has[emit_mi.dst.reg] = {};
                        reg_has[emit_mi.dst.reg].has_imm32 = true;
                        reg_has[emit_mi.dst.reg].imm32_val = emit_mi.src1.value;
                    } else if (emit_mi.dst.kind == MOperandKind::REG) {
                        invalidate_reg(emit_mi.dst.reg);
                    }
                } else if (emit_mi.op == MOp::CMP
                        || emit_mi.op == MOp::TEST
                        || emit_mi.op == MOp::JMP
                        || emit_mi.op == MOp::JCC
                        || emit_mi.op == MOp::RET) {
                    /* CMP/TEST: solo afectan flags, no modifican dst.
                     * JMP/JCC/RET: control flow, no escriben regs. */
                } else if (emit_mi.dst.kind == MOperandKind::REG) {
                    invalidate_reg(emit_mi.dst.reg);
                } else if (emit_mi.dst.kind == MOperandKind::MEM) {
                    if (is_slot_mem(emit_mi.dst)) {
                        invalidate_slot_holders(emit_mi.dst);
                    } else {
                        invalidate_all();
                    }
                }

                /* CALL clobera todos caller-saved + posible mem aliasing. */
                if (emit_mi.op == MOp::CALL) {
                    invalidate_all();
                }

                out.push_back(emit_mi);
            }
            block.instrs = std::move(out);
            /* Guardar exit_state para el cross-block inheritance. */
            for (int r = 0; r < 64; ++r) {
                exit_states[bi][r] = reg_has[r];
            }
        }

        /* Dead Store Elimination (DSE) DESPUES del peephole.
         *
         * Para cada slot SSA puro (RBP+disp con index=63), si NADIE lo lee
         * en la funcion completa, todos los STOREs a el son dead.
         * Ejecutamos DESPUES del peephole porque el peephole elimina
         * LOADs redundantes (e.g. convierte mov reg, [slot] inmediatamente
         * tras un mov [slot], reg en skip), dejando slots dead que el DSE
         * puede limpiar.
         *
         * Beneficio: elimina muchos slots de SSA values intermedios que
         * solo se usan localmente (el peephole los mantuvo en reg). */
        {
            const uint8_t RBP_REG_DSE = reg_id(MReg::RBP);
            auto is_pure_slot = [&](const MOperand &op) -> bool {
                return op.kind == MOperandKind::MEM
                    && op.reg == RBP_REG_DSE
                    && (op.width & 0xFC) == 0xFC;
            };
            std::unordered_set<int32_t> read_slots;
            for (const auto &block : mf.blocks) {
                for (const auto &mi : block.instrs) {
                    if (mi.src1.kind == MOperandKind::MEM && is_pure_slot(mi.src1)) {
                        read_slots.insert(mi.src1.value);
                    }
                    if (mi.src2.kind == MOperandKind::MEM && is_pure_slot(mi.src2)) {
                        read_slots.insert(mi.src2.value);
                    }
                }
            }
            size_t dse_removed = 0;
            for (auto &block : mf.blocks) {
                std::vector<MInstr> out;
                out.reserve(block.instrs.size());
                for (const auto &mi : block.instrs) {
                    if (mi.op == MOp::MOV
                     && mi.dst.kind == MOperandKind::MEM
                     && is_pure_slot(mi.dst)
                     && read_slots.find(mi.dst.value) == read_slots.end()) {
                        ++dse_removed;
                        continue;
                    }
                    out.push_back(mi);
                }
                block.instrs = std::move(out);
            }
            (void)dse_removed;
        }
        }  /* end scope peephole+DSE */

        /* Phase D.7: rewrite slot->reg + destructive in-place peephole.
         *
         * Orden FINAL determinado empiricamente (16ms wall en bench):
         *   1. (arriba) Selector emite slot-based.
         *   2. (arriba) Peephole + DSE optimiza slot-based.
         *   3. (aqui)   Rewrite slot->reg sobre VIDs pinned.
         *   4. (aqui)   Destructive in-place peephole (3-instr window:
         *               mov scratch, regX; ALU; mov regX, scratch
         *               -> ALU regX, src; con INC/DEC para +/-1).
         *
         * Si @c regalloc.empty() todo este bloque es no-op. */
        if (!regalloc.empty()) {
            apply_jit_regalloc_rewrite(mf, ir_fn, regalloc);

            /* Helpers para el destructive peephole. */
            auto is_callee_saved_pinned = [&](uint8_t reg_id) -> bool {
                for (MReg cs : regalloc.callee_saved_used) {
                    if (static_cast<uint8_t>(cs) == reg_id) return true;
                }
                return false;
            };
            auto is_scratch_reg = [](uint8_t reg_id) -> bool {
                return reg_id == static_cast<uint8_t>(MReg::RAX)
                    || reg_id == static_cast<uint8_t>(MReg::RCX)
                    || reg_id == static_cast<uint8_t>(MReg::RDX);
            };
            auto is_inplace_alu = [](MOp op) -> bool {
                return op == MOp::ADD || op == MOp::SUB
                    || op == MOp::AND || op == MOp::OR
                    || op == MOp::XOR || op == MOp::IMUL;
            };

            for (auto &block : mf.blocks) {
                std::vector<MInstr> out;
                out.reserve(block.instrs.size());

                /* Tracking de equivalencias reg ≡ reg dentro del bloque.
                 *
                 * El peephole previo hace copy-prop: cuando ve @c mov
                 * rcx,r12 seguido de uso de @c r12, propaga rcx en lugar.
                 * Resultado: el patron destructivo aparece como
                 *   mov rax, rcx   (donde rcx ≡ r12)
                 *   add rax, 1
                 *   mov r12, rax
                 * Sin equivalence tracking, el m0.src1.reg = rcx no
                 * matchea con m2.dst.reg = r12.  Con tracking, tratamos
                 * rcx como equivalent a r12 en este punto. */
                std::vector<int8_t> reg_equiv(32, -1);  /* equiv[r] = r' o -1 */
                auto reg_equiv_root = [&](uint8_t r) -> uint8_t {
                    /* Path-compress trivial: 1 nivel ya basta porque
                     * registramos copias directas. */
                    if (r < reg_equiv.size() && reg_equiv[r] >= 0) {
                        return static_cast<uint8_t>(reg_equiv[r]);
                    }
                    return r;
                };
                auto invalidate_reg = [&](uint8_t r) {
                    if (r < reg_equiv.size()) reg_equiv[r] = -1;
                    /* Tambien invalida cualquier reg que apunte a r. */
                    for (size_t k = 0; k < reg_equiv.size(); ++k) {
                        if (reg_equiv[k] == static_cast<int8_t>(r)) {
                            reg_equiv[k] = -1;
                        }
                    }
                };

                size_t i = 0;
                while (i < block.instrs.size()) {
                    if (i + 2 >= block.instrs.size()) {
                        const MInstr &mi = block.instrs[i];
                        /* Actualizar tracking de equiv para esta instr. */
                        if (mi.op == MOp::MOV
                         && mi.dst.kind == MOperandKind::REG
                         && mi.src1.kind == MOperandKind::REG
                         && mi.dst.reg < 32 && mi.src1.reg < 32) {
                            invalidate_reg(mi.dst.reg);
                            reg_equiv[mi.dst.reg] =
                                static_cast<int8_t>(mi.src1.reg);
                        } else if (mi.dst.kind == MOperandKind::REG) {
                            invalidate_reg(mi.dst.reg);
                        }
                        out.push_back(mi);
                        ++i;
                        continue;
                    }
                    const MInstr &m0 = block.instrs[i];
                    const MInstr &m1 = block.instrs[i + 1];
                    const MInstr &m2 = block.instrs[i + 2];
                    bool m0_ok = m0.op == MOp::MOV
                        && m0.dst.kind == MOperandKind::REG
                        && m0.src1.kind == MOperandKind::REG
                        && is_scratch_reg(m0.dst.reg);
                    /* Permitir m0.src1 pinned directamente O equivalente
                     * a un reg pinned via copy-prop previa. */
                    uint8_t src_root = m0_ok
                        ? reg_equiv_root(m0.src1.reg)
                        : 0;
                    if (m0_ok && !is_callee_saved_pinned(m0.src1.reg)) {
                        if (!is_callee_saved_pinned(src_root)) {
                            m0_ok = false;
                        }
                    }
                    /* Si m0.src1 es alias, usar el root para comparar
                     * con m2.dst. */
                    bool m2_ok = m0_ok
                        && m2.op == MOp::MOV
                        && m2.dst.kind == MOperandKind::REG
                        && m2.src1.kind == MOperandKind::REG
                        && (m2.dst.reg == m0.src1.reg
                            || m2.dst.reg == src_root)
                        && m2.src1.reg == m0.dst.reg;
                    bool m1_ok = m2_ok
                        && is_inplace_alu(m1.op)
                        && m1.dst.kind == MOperandKind::REG
                        && m1.dst.reg == m0.dst.reg
                        && m1.src1.kind != MOperandKind::NONE;
                    if (m1_ok && m1.src1.kind == MOperandKind::REG
                     && m1.src1.reg == m0.dst.reg) {
                        m1_ok = false;
                    }
                    if (m1_ok) {
                        const uint8_t regX_id = m2.dst.reg;
                        MOp final_op = m1.op;
                        MOperand src_final = m1.src1;
                        if (m1.src1.kind == MOperandKind::IMM32) {
                            if (m1.op == MOp::ADD && m1.src1.value == 1) {
                                final_op = MOp::INC;
                                src_final.kind = MOperandKind::NONE;
                            } else if (m1.op == MOp::SUB && m1.src1.value == 1) {
                                final_op = MOp::DEC;
                                src_final.kind = MOperandKind::NONE;
                            } else if (m1.op == MOp::ADD && m1.src1.value == -1) {
                                final_op = MOp::DEC;
                                src_final.kind = MOperandKind::NONE;
                            } else if (m1.op == MOp::SUB && m1.src1.value == -1) {
                                final_op = MOp::INC;
                                src_final.kind = MOperandKind::NONE;
                            }
                        }
                        MInstr collapsed;
                        collapsed.op = final_op;
                        collapsed.dst = MOperand::make_reg(
                            static_cast<MReg>(regX_id),
                            m2.dst.width);
                        collapsed.src1 = src_final;
                        collapsed.source_pc = m1.source_pc;
                        /* Invalidar equiv para regX (su valor cambia). */
                        invalidate_reg(regX_id);
                        out.push_back(collapsed);
                        i += 3;
                        continue;
                    }
                    /* No matchea -- emitir m0 y actualizar tracking. */
                    if (m0.op == MOp::MOV
                     && m0.dst.kind == MOperandKind::REG
                     && m0.src1.kind == MOperandKind::REG
                     && m0.dst.reg < 32 && m0.src1.reg < 32) {
                        invalidate_reg(m0.dst.reg);
                        reg_equiv[m0.dst.reg] = static_cast<int8_t>(m0.src1.reg);
                    } else if (m0.dst.kind == MOperandKind::REG
                            && m0.dst.reg < 32) {
                        invalidate_reg(m0.dst.reg);
                    }
                    /* Cualquier CALL invalida TODOS los caller-saved. */
                    if (m0.op == MOp::CALL || m0.op == MOp::SAFEPOINT) {
                        for (size_t k = 0; k < reg_equiv.size(); ++k) {
                            reg_equiv[k] = -1;
                        }
                    }
                    out.push_back(m0);
                    ++i;
                }
                block.instrs = std::move(out);
            }

            /* Cleanup post-destructive: eliminar dead reg writes y
             * self-copy round-trips que quedan tras coalescing.
             *
             * Patron self-copy (PHI coalesce leftover):
             *   mov rax, r13      ; m0: scratch <- reg
             *   mov r13, rax      ; m1: reg <- scratch (mismo reg, mismo scratch)
             *   -> ELIMINAR ambos (no-op si scratch dead despues)
             *
             * Patron dead reg write:
             *   mov rax, imm32    ; rax = X
             *   mov rax, ...      ; rax = Y  (sin intervening read de rax)
             *   -> ELIMINAR el primero
             *
             * Iteramos UNA vez por bloque.  Para mayor agresividad
             * podriamos iterar hasta fixed point, pero una pasada
             * captura los casos comunes de la coalesce. */
            for (auto &block : mf.blocks) {
                std::vector<MInstr> out;
                out.reserve(block.instrs.size());
                for (size_t k = 0; k < block.instrs.size(); ++k) {
                    const MInstr &cur = block.instrs[k];
                    /* Self-copy pair: m_k+1 reverses m_k. */
                    if (k + 1 < block.instrs.size()) {
                        const MInstr &nxt = block.instrs[k + 1];
                        if (cur.op == MOp::MOV
                         && nxt.op == MOp::MOV
                         && cur.dst.kind == MOperandKind::REG
                         && cur.src1.kind == MOperandKind::REG
                         && nxt.dst.kind == MOperandKind::REG
                         && nxt.src1.kind == MOperandKind::REG
                         && cur.dst.reg == nxt.src1.reg
                         && cur.src1.reg == nxt.dst.reg) {
                            /* No-op pair.  Skip ambos. */
                            ++k;  /* skip nxt */
                            continue;
                        }
                    }
                    /* Dead reg write: m_k es @c mov reg, X y una instr
                     * posterior en el mismo bloque escribe el mismo reg
                     * en forma PURA (sin leerlo antes), antes de que
                     * nadie lo lea.
                     *
                     * SUPER-CONSERVATIVO en v1: solo aplicamos a @c MOV
                     * con src1 IMM32/IMM64.  Los casos comunes que esto
                     * captura son @c mov rax, imm32 muerto (de un CONST
                     * de IR que el imm fold ya reemplazo).  Casos mas
                     * generales pueden romper correctness por dependencias
                     * sutiles a traves de bloques o efectos secundarios. */
                    if (cur.op == MOp::MOV
                     && cur.dst.kind == MOperandKind::REG
                     && (cur.src1.kind == MOperandKind::IMM32
                      || cur.src1.kind == MOperandKind::IMM64_IDX)) {
                        bool dead = false;
                        const uint8_t target_reg = cur.dst.reg;
                        auto reads_reg = [&](const MOperand &op) {
                            return (op.kind == MOperandKind::REG
                                    && op.reg == target_reg)
                                || (op.kind == MOperandKind::MEM
                                    && op.reg == target_reg);
                        };
                        for (size_t j = k + 1; j < block.instrs.size(); ++j) {
                            const MInstr &later = block.instrs[j];
                            /* Lectura via src1/src2: break (no dead). */
                            if (reads_reg(later.src1) || reads_reg(later.src2)) {
                                break;
                            }
                            /* dst como MEM con base = target_reg: lectura. */
                            if (later.dst.kind == MOperandKind::MEM
                             && later.dst.reg == target_reg) {
                                break;
                            }
                            /* dst REG == target_reg: depende del op. */
                            if (later.dst.kind == MOperandKind::REG
                             && later.dst.reg == target_reg) {
                                /* Ops que ESCRIBEN dst sin leerlo: pure write. */
                                bool pure_write = false;
                                switch (later.op) {
                                    case MOp::MOV:
                                    case MOp::MOVZX:
                                    case MOp::MOVSX:
                                    case MOp::LEA:
                                    case MOp::POP:
                                    case MOp::SETCC:
                                        pure_write = true;
                                        break;
                                    default:
                                        /* ALU (ADD/SUB/AND/OR/XOR/IMUL/
                                         * NEG/NOT/SHL/SHR/SAR/INC/DEC),
                                         * CMP, TEST, IDIV, CQO, CMOVCC,
                                         * PUSH, JMP/JCC/CALL con reg
                                         * target -> LEEN dst.  No es dead. */
                                        break;
                                }
                                if (pure_write) {
                                    dead = true;
                                }
                                break;  /* matched dst.reg, done. */
                            }
                            /* Conservative: parar en cualquier control flow
                             * o call.  Tambien IDIV/CQO que clobean rax/rdx
                             * implicitamente. */
                            if (later.op == MOp::CALL
                             || later.op == MOp::SAFEPOINT
                             || later.op == MOp::JMP
                             || later.op == MOp::JCC
                             || later.op == MOp::RET
                             || later.op == MOp::IDIV
                             || later.op == MOp::CQO) {
                                /* Si target_reg es rax/rdx, IDIV/CQO lo
                                 * tocan.  Conservadoramente parar. */
                                break;
                            }
                        }
                        if (dead) continue;
                    }
                    out.push_back(cur);
                }
                block.instrs = std::move(out);
            }
        }

        if (out_unsupported) *out_unsupported = unsupported;
        return mf;
    }

} // namespace jit
