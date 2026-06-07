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

        /** @brief Replica de @c EmitCtx::sanitize del ir_emitter: convierte
         *  cualquier caracter no-alfanumerico/no-underscore en '_'.  Usado
         *  para resolver nombres de funcion en el symbol_table del .velb,
         *  que el linker emite ya sanitizados (igual que el .vel emitido). */
        inline std::string sanitize_label_name(const std::string &s) {
            std::string r;
            r.reserve(s.size());
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') r += c;
                else r += '_';
            }
            return r;
        }

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

        /* Calling convention nativa per-platform.  JIT_PROC_REG es donde
         * el prologue VM_ABI deposita el ProcessVM* (callee-saved RBX,
         * vive durante toda la funcion).  NATIVE_ARG[0..2] son los
         * primeros 3 args segun ABI (SysV: rdi/rsi/rdx; Win64:
         * rcx/rdx/r8). */
        constexpr MReg JIT_PROC_REG = MReg::RBX;
#if defined(_WIN32)
        constexpr MReg NATIVE_ARG0 = MReg::RCX;
        constexpr MReg NATIVE_ARG1 = MReg::RDX;
        constexpr MReg NATIVE_ARG2 = MReg::R8;
#else
        constexpr MReg NATIVE_ARG0 = MReg::RDI;
        constexpr MReg NATIVE_ARG1 = MReg::RSI;
        constexpr MReg NATIVE_ARG2 = MReg::RDX;
#endif

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

        /**
         * @brief Sprint string-perf-6: variante de load_op que REMATERIALIZA
         * constantes pequenas como inmediato en lugar de cargarlas del slot
         * stack en cada uso.  Cierra una regresion grave (~30-50%) en hot
         * loops con muchas constantes (e.g. branch_unpredict tenia el `0`
         * y el `1` cargados del stack 4 veces por iter).
         *
         * Seguro tras Sprint LANG.fix-5 porque los CONSTS estan EXCLUIDOS
         * del regalloc (`val.is_const -> vi.excluded = true`) y por tanto
         * no pueden coalescer con phis: el bug previo de corrupcion no
         * aplica aqui.
         *
         * Estrategia:
         *   - Si val no es CONST: load normal `mov reg, [slot]`.
         *   - Si CONST con valor 0: `xor reg, reg` (3 bytes vs 7-9 del load).
         *   - Si CONST cabe en imm32 signed: `mov reg, imm32` (5-6 bytes).
         *   - Si CONST cabe en imm64: `mov reg, imm64` (10 bytes; mejor que
         *     un load si el slot esta en cache cold, equivalente si caliente).
         */
        void load_op_rematerializable(MFunction &mf, const ir::IrFunction &fn,
                                       ir::IrValueId vid, MReg dst) {
            if (vid < fn.values.size()) {
                const auto &val = fn.values[vid];
                if (val.is_const) {
                    const int64_t cv = static_cast<int64_t>(val.const_val);
                    if (cv == 0) {
                        /* xor dst, dst -> rax = 0 en 2-3 bytes, sin flags
                         * problem porque el caller emite cmp inmediatamente
                         * despues (su flags es lo que importa). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_binary(MOp::XOR,
                                MOperand::make_reg(dst),
                                MOperand::make_reg(dst),
                                MOperand::make_reg(dst)));
                        return;
                    }
                    if (cv >= INT32_MIN && cv <= INT32_MAX) {
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(dst),
                                MOperand::make_imm32(static_cast<int32_t>(cv))));
                        return;
                    }
                    /* imm64: usar pool. */
                    const uint32_t idx = mf.intern_imm64(val.const_val);
                    mf.blocks.back().instrs.push_back(
                        MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(dst),
                            MOperand::make_imm64_idx(idx)));
                    return;
                }
            }
            /* Default: load del slot. */
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

        /**
         * @brief callback-ABI: true si @p op es "hoja-segura", es decir,
         *        NO escribe @c proc->registers.regs[0..15] ni reentra a la
         *        ejecucion de la VM.
         *
         * Para una funcion-callback cuyo cuerpo solo contiene ops hoja-seguras,
         * el prologo NO necesita salvar el banco de registros VM (save-set
         * vacio), porque los args entran directo a los slots de params y el
         * cuerpo nunca toca @c proc->registers[].  Cualquier op fuera de esta
         * whitelist fuerza el modo "safe" (salvar/restaurar R0..R15, igual que
         * el thunk previo).  Whitelist conservadora: una op no listada ->
         * safe (correcto, solo sin speedup).
         *
         * Excluidas (fuerzan safe): toda la familia CALL (marshalling +
         * reentrada), RAW_ASM (arbitrario), READ_VM_REG (lee proc->regs;
         * en fast-mode los args no estan ahi), y por defecto cualquier op
         * no enumerada (OOP dinamico, async, distrib, sync, reflexion, etc.).
         */
        inline bool cb_is_leaf_safe_op(ir::IrOp op) noexcept {
            switch (op) {
                /* constantes / movimiento */
                case ir::IrOp::CONST: case ir::IrOp::MOV: case ir::IrOp::NOP:
                case ir::IrOp::STR_LIT_ADDR: case ir::IrOp::LABEL_ADDR:
                /* aritmetica entera + extendida */
                case ir::IrOp::ADD: case ir::IrOp::SUB: case ir::IrOp::MUL:
                case ir::IrOp::DIV: case ir::IrOp::MOD: case ir::IrOp::NEG:
                case ir::IrOp::IABS: case ir::IrOp::IMIN: case ir::IrOp::IMAX:
                case ir::IrOp::IMINU: case ir::IrOp::IMAXU: case ir::IrOp::ILOG2:
                /* aritmetica flotante */
                case ir::IrOp::FADD: case ir::IrOp::FSUB: case ir::IrOp::FMUL:
                case ir::IrOp::FDIV: case ir::IrOp::FNEG: case ir::IrOp::FABS:
                case ir::IrOp::FSQRT: case ir::IrOp::FMIN: case ir::IrOp::FMAX:
                case ir::IrOp::FFLOOR: case ir::IrOp::FCEIL: case ir::IrOp::FROUND:
                case ir::IrOp::FTRUNC:
                /* logica / shifts / bit ops */
                case ir::IrOp::AND: case ir::IrOp::OR: case ir::IrOp::XOR:
                case ir::IrOp::NOT: case ir::IrOp::SHL: case ir::IrOp::SHR:
                case ir::IrOp::SAR: case ir::IrOp::CLZ: case ir::IrOp::CTZ:
                case ir::IrOp::POPCNT: case ir::IrOp::BYTESWAP:
                case ir::IrOp::ROTL: case ir::IrOp::ROTR:
                /* comparaciones */
                case ir::IrOp::CMP_EQ: case ir::IrOp::CMP_NE: case ir::IrOp::CMP_LT:
                case ir::IrOp::CMP_GT: case ir::IrOp::CMP_LE: case ir::IrOp::CMP_GE:
                case ir::IrOp::CMP_ULT: case ir::IrOp::CMP_UGT: case ir::IrOp::CMP_ULE:
                case ir::IrOp::CMP_UGE:
                case ir::IrOp::FCMP_EQ: case ir::IrOp::FCMP_NE: case ir::IrOp::FCMP_LT:
                case ir::IrOp::FCMP_GT: case ir::IrOp::FCMP_LE: case ir::IrOp::FCMP_GE:
                /* conversiones */
                case ir::IrOp::CAST: case ir::IrOp::ZEXT: case ir::IrOp::SEXT:
                case ir::IrOp::TRUNC: case ir::IrOp::ITOF: case ir::IrOp::UITOF:
                case ir::IrOp::FTOI: case ir::IrOp::FTOUI: case ir::IrOp::F32TOF64:
                case ir::IrOp::F64TOF32: case ir::IrOp::BITCAST:
                /* control de flujo */
                case ir::IrOp::BR: case ir::IrOp::BR_COND: case ir::IrOp::RET:
                case ir::IrOp::UNREACHABLE: case ir::IrOp::PHI:
                /* memoria (LOAD/STORE usan page-cache+RBX+scratch; ALLOCA
                 * toca proc->stack_pointer, manejado aparte por fn_has_alloca) */
                case ir::IrOp::ALLOCA: case ir::IrOp::LOAD: case ir::IrOp::STORE:
                case ir::IrOp::GETFIELD: case ir::IrOp::SETFIELD:
                case ir::IrOp::ISNULL: case ir::IrOp::GEP:
                case ir::IrOp::GETPROC:
                    return true;
                default:
                    return false;
            }
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

        /* Phase D.7-exc (2026-06-04): exception handling.  El modelo v1 del
         * throw (do_throw modifica proc->rip + epilogue + el scheduler reanuda
         * el handler en INTERP) SOLO funciona si el catch esta en bytecode
         * separado.  Cuando el metodo se JIT-compila ENTERO, el handler vive en
         * el frame JIT y sus valores SSA estan en slots del HOST stack que el
         * interp no ve -> estado corrupto -> segfault (caso 30_unwrap_null).
         *
         * Hasta el sprint dedicado de unwinding host-aware (landing pad host +
         * longjmp manual, parte de D.13), los metodos con TRYENTER caen a
         * interp -- que maneja las excepciones correctamente.  CERO coste para
         * el happy path: los metodos sin try/catch tienen JIT completo; las
         * excepciones son el camino EXCEPCIONAL (raro y ya lento por diseno). */
        for (const auto &blk : ir_fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.op == ir::IrOp::TRYENTER) {
                    if (out_unsupported) *out_unsupported = true;
                    return mf;
                }
            }
        }

        /* C2-cimiento (2026-06-07): clave de call site para IC slots
         * DIRECCIONABLES.  fn_pc se resuelve una vez (via el symbol_table del
         * .velb); el ordinal incrementa por cada IC reservado en la funcion,
         * en el orden deterministico en que el selector recorre las instrs.
         * Asi el mismo call site obtiene la misma clave entre recompilaciones
         * -> el pase C2 puede encontrar el slot y leer las clases observadas.
         * Si no hay symbol_table (NATIVE_ABI/tests) -> clave 0 = slot fresco
         * no-direccionable (comportamiento previo). */
        const uint64_t ic_fn_pc =
            (opts_.resolve_symbol && !ir_fn.name.empty())
                ? opts_.resolve_symbol("code." + ir_fn.name)
                : 0;
        uint32_t ic_ordinal = 0;
        auto next_ic_key = [&]() -> uint64_t {
            const uint32_t o = ic_ordinal++;
            return ic_fn_pc ? ((ic_fn_pc << 8) | (o & 0xFFu)) : 0;
        };

        /* C2 tier-up (2026-06-07): decidir si esta funcion lleva contador
         * on-entry de tier-up.  Condiciones:
         *   - C2 activo (tier_up_handler_addr + threshold + reserve_tier_counter),
         *   - VM_ABI sin entry de callback (el conteo + recompile asume VM_ABI),
         *   - fn_pc resoluble (clave estable para el contador y el recompile),
         *   - y (tier_up_all_fns || la funcion tiene >=1 CALLVIRT).  Por defecto
         *     solo se instrumentan las funciones con CALLVIRT (candidato real a
         *     especulacion); las hojas/aritmetica no pagan nada.
         * El contador (8 bytes, keyed por fn_pc) se reserva aqui; su direccion
         * se embebe en el prologo mas abajo. */
        uint64_t tier_counter_addr = 0;
        if (opts_.tier_up_handler_addr != 0 && opts_.tier_up_threshold != 0
         && opts_.reserve_tier_counter && ic_fn_pc != 0
         && opts_.mode == SelectorMode::VM_ABI && !opts_.callback_entry) {
            bool has_callvirt = opts_.tier_up_all_fns;
            if (!has_callvirt) {
                for (const auto &blk : ir_fn.blocks) {
                    for (const auto &ins : blk.instrs) {
                        if (ins.op == ir::IrOp::CALLVIRT) { has_callvirt = true; break; }
                    }
                    if (has_callvirt) break;
                }
            }
            if (has_callvirt) {
                tier_counter_addr = opts_.reserve_tier_counter(ic_fn_pc);
            }
        }

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
        /* Phase D.jit-mem-model PARTIAL: safety check + dataflow.
         *
         * Cubrimos el problema mas comun (ALLOCA en JIT produce host
         * pero IR lo marca como VM-addr) via dataflow forward; el
         * resto requiere un sprint dedicado mas grande con:
         *   1. Bidirectional boundary translation interp<->JIT.
         *   2. Runtime entry vm_translate(proc, vaddr) -> host_ptr.
         *   3. Posiblemente tagging runtime de ptrs (bit alto).
         *
         * Mientras tanto: si una funcion tiene cualquier LOAD/STORE sobre
         * un puntero que NO se prueba como host_in_jit, abortamos JIT
         * para esa funcion. El interp maneja vm_mem correctamente via
         * VirtualMemory.  Funciones puramente host-pointers (malloc/new/
         * fields de objetos GC) JIT-compilan sin problemas.
         *
         * Pre-pase del dataflow forward para clasificar cada SSA value
         * como "host_ptr en runtime JIT":
         *
         * Por que: el IR fue disenado para el interp donde:
         *   - ALLOCA produce un VM-addr (slot en `vm_mem` stack).
         *   - is_host_ptr=true solo para malloc/new/gc_deref/etc.
         *
         * En JIT, ALLOCA emite `sub rsp, N` en pila HOST.  El slot
         * dst tiene un host_ptr aunque el IR diga is_host_ptr=false.
         * Asi que necesitamos un dataflow LOCAL al selector que
         * propague host_in_jit forward sin tocar el IR.
         *
         * Seeds:
         *   - Cualquier valor con IR is_host_ptr=true.
         *   - Cualquier ALLOCA dst (JIT usa rsp host).
         *   - Cualquier op que produzca host_ptr por construccion:
         *     GC_ALLOC, GC_ALLOCP, GC_DEREF_HOST, NEWOBJ, RAW_ALLOC,
         *     STR_LIT_ADDR.
         *   - Params PTR: asumimos JIT->JIT calling convention (caller
         *     paso un host_ptr).  Si el caller es interp con VM-addr,
         *     el callee crashea; ese caso es raro y se contiene en
         *     la suite e2e con tests reales.
         *
         * Propagacion (fixed-point):
         *   - ADD/SUB de un host_ptr -> host_ptr (aritmetica de ptr).
         *   - BITCAST/MOV/CAST/SEXT/ZEXT/TRUNC: preserva host-ness
         *     del operando.
         *   - PHI: host_ptr si TODOS los args son host (conservador).
         *   - LOAD: solo host si el IR original lo marca (raro --
         *     casi siempre el LOAD i32/i64 produce valor, no ptr).
         *
         * Resultado: vector<bool> host_in_jit indexado por VID.  Se
         * consulta en LOAD/STORE para elegir entre native mov (host)
         * y fallback vm_read/write (VM-addr). */
        std::vector<uint8_t> host_in_jit(ir_fn.values.size(), 0u);

        /* Sprint mem-loop-fix (2026-06-02): identificar VIDs que vienen
         * de un ALLOCA con `host_alloca && host_alloca_explicit_free`.
         * En el JIT, esos ALLOCAs emiten `sub rsp, N` (host stack), asi
         * que el RAW_FREE asociado (preservado por el promote pass para
         * que el INTERP no acumule htrack) debe ser SKIPPED en el JIT.
         * Llamar `vrt_raw_free` sobre un ptr de host stack es crash
         * garantizado.  El set se llena en el pre-pase y se consulta
         * en el case IrOp::RAW_FREE. */
        std::vector<uint8_t> skip_raw_free_vid(ir_fn.values.size(), 0u);

        /* Sprint mem-loop-fix-v2 (2026-06-02): tamano del ALLOCA por VID
         * para emitir `add rsp, aligned(N)` en el RAW_FREE matching,
         * manteniendo el host stack balanceado per-iteracion en loops. */
        std::vector<uint64_t> alloca_size_by_vid(ir_fn.values.size(), 0ull);

        /* Seed inicial.  Tres categorias:
         *   1. Valores con IR is_host_ptr=true: marcados por el frontend
         *      (malloc/new/fields GC).  Son host_ptr en TODO modelo.
         *   2. ALLOCA dsts + GC_ALLOC/etc: en JIT el ALLOCA emite
         *      sub rsp host, asi que el dst es host_ptr aunque el IR
         *      lo marque is_host_ptr=false (convencion interp).
         *   3. Params PTR: asumimos JIT->JIT convention (caller paso
         *      host_ptr). Trampoline JIT->interp con arg host_ptr
         *      aborta el caller, asi que el modelo es coherente:
         *      cuando una funcion JIT-compila, TODOS sus callees
         *      alcanzables tambien estan en JIT (eager-compile
         *      cascade); o el caller cae a interp si algun callee
         *      no es JIT-able.
         *
         * El unico caso problematico es interp->JIT dispatch (dispatch
         * hook con `enter_jit`): si interp pasa una VM-addr a un JIT
         * callee que espera host_ptr, el inline cache miss + fallback
         * vm_read trata el host_ptr como VM-addr -> garbage.  En la
         * practica casi no ocurre con eager-compile cascade activo. */
        for (size_t vid = 0; vid < ir_fn.values.size(); ++vid) {
            if (ir_fn.values[vid].is_host_ptr) host_in_jit[vid] = 1u;
        }
        /* NO seedeamos params PTR como host_in_jit: el caller puede ser
         * interp (que pasa VM-addrs) o JIT (que pasa host_ptrs).  Sin
         * tag runtime no podemos decidir.  Asumimos VM-addr conservativo;
         * el LOAD/STORE de esos params triggerea el bypass abajo y la
         * funcion cae a interp.  Sprint Phase D.jit-mem-model VM-STACK
         * dedicado lo resolveria cambiando ALLOCA del JIT a VM-stack
         * (uniforme con interp), eliminando el mixing por completo. */
        for (const auto &blk : ir_fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == ir::IR_NO_VALUE
                 || ins.dst >= host_in_jit.size()) continue;
                switch (ins.op) {
                    /* ALLOCA con host_alloca=true (AUTO-PROMOTE): el JIT
                     * lo emite en host stack, asi el dst es genuino
                     * host_ptr.  Seedear como host_in_jit hace que LOAD/
                     * STORE sobre derivados use native mov directo (sin
                     * inline cache). */
                    case ir::IrOp::ALLOCA:
                        if (ins.host_alloca) {
                            host_in_jit[ins.dst] = 1u;
                            /* Sprint mem-loop-fix-v2: AMBOS casos
                             * (con/sin explicit_free) emiten `sub rsp, N`
                             * en host stack.  La diferencia es que el
                             * explicit_free TAMBIEN debe emitir `add rsp, N`
                             * en el RAW_FREE matching para no acumular
                             * stack en loops.  Marcamos skip_raw_free_vid
                             * + guardamos size para el RAW_FREE. */
                            if (ins.host_alloca_explicit_free
                             && ins.dst < skip_raw_free_vid.size()) {
                                skip_raw_free_vid[ins.dst] = 1u;
                                alloca_size_by_vid[ins.dst] = ins.imm;
                            }
                        }
                        break;
                    case ir::IrOp::GC_ALLOC:
                    case ir::IrOp::GC_ALLOCP:
                    case ir::IrOp::GC_DEREF_HOST:
                    case ir::IrOp::NEWOBJ:
                    case ir::IrOp::RAW_ALLOC:
                        host_in_jit[ins.dst] = 1u;
                        break;
                    /* STR_LIT_ADDR produce un VM-addr (offset al slot de
                     * static_data en proc->vm_mem), NO un host_ptr.  Si lo
                     * seedeamos como host_in_jit, el LOAD/STORE subsiguiente
                     * emitiria un native mov directo y page-fault.  Dejar
                     * SIN marca: el LOAD/STORE caera al inline cache +
                     * fallback vrt_vm_read/write que SI maneja VM-addrs. */
                    case ir::IrOp::STR_LIT_ADDR:
                        /* host_in_jit[ins.dst] permanece 0. */
                        break;
                    default: break;
                }
            }
        }

        /* Propagacion forward fixed-point.  Cota de 8 iter (en practica
         * 2-3 son suficientes; cualquier programa razonable converge). */
        for (int iter = 0; iter < 8; ++iter) {
            bool changed = false;
            for (const auto &blk : ir_fn.blocks) {
                for (const auto &ins : blk.instrs) {
                    if (ins.dst == ir::IR_NO_VALUE
                     || ins.dst >= host_in_jit.size()) continue;
                    if (host_in_jit[ins.dst]) continue;

                    auto any_op_host = [&]() {
                        for (ir::IrValueId v : ins.operands) {
                            if (v != ir::IR_NO_VALUE && v < host_in_jit.size()
                             && host_in_jit[v]) return true;
                        }
                        return false;
                    };

                    /* Sprint mem-loop-fix: propaga skip_raw_free_vid
                     * cuando un operand viene de un ALLOCA con
                     * host_alloca_explicit_free.  Cualquier deriv
                     * (BITCAST/ADD/etc) hereda el flag para que el
                     * RAW_FREE sobre el deriv tambien se skipee. */
                    auto any_op_skip_free = [&]() {
                        for (ir::IrValueId v : ins.operands) {
                            if (v != ir::IR_NO_VALUE
                             && v < skip_raw_free_vid.size()
                             && skip_raw_free_vid[v]) return true;
                        }
                        return false;
                    };

                    /* Sprint mem-loop-fix-v2: hereda size del operand
                     * para que el RAW_FREE sobre el deriv sepa cuanto
                     * add rsp emitir. */
                    auto inherit_alloca_size = [&]() -> uint64_t {
                        for (ir::IrValueId v : ins.operands) {
                            if (v != ir::IR_NO_VALUE
                             && v < alloca_size_by_vid.size()
                             && alloca_size_by_vid[v] != 0) {
                                return alloca_size_by_vid[v];
                            }
                        }
                        return 0;
                    };

                    switch (ins.op) {
                        case ir::IrOp::ADD:
                        case ir::IrOp::SUB:
                        case ir::IrOp::BITCAST:
                        case ir::IrOp::MOV:
                        case ir::IrOp::CAST:
                        case ir::IrOp::ZEXT:
                        case ir::IrOp::SEXT:
                        case ir::IrOp::TRUNC:
                            if (any_op_host()) {
                                host_in_jit[ins.dst] = 1u;
                                changed = true;
                            }
                            if (ins.dst < skip_raw_free_vid.size()
                             && !skip_raw_free_vid[ins.dst]
                             && any_op_skip_free()) {
                                skip_raw_free_vid[ins.dst] = 1u;
                                if (ins.dst < alloca_size_by_vid.size()
                                 && alloca_size_by_vid[ins.dst] == 0) {
                                    alloca_size_by_vid[ins.dst] =
                                        inherit_alloca_size();
                                }
                                changed = true;
                            }
                            break;
                        case ir::IrOp::PHI: {
                            if (ins.phi_args.empty()) break;
                            bool all_host = true;
                            for (const auto &pa : ins.phi_args) {
                                if (pa.value == ir::IR_NO_VALUE
                                 || pa.value >= host_in_jit.size()
                                 || !host_in_jit[pa.value]) {
                                    all_host = false;
                                    break;
                                }
                            }
                            if (all_host) {
                                host_in_jit[ins.dst] = 1u;
                                changed = true;
                            }
                            break;
                        }
                        default: break;
                    }
                }
            }
            if (!changed) break;
        }

        /* Phase D.jit-mem-model VM-STACK: el bypass que abortaba JIT
         * para LOAD/STORE !host_in_jit ya NO es necesario.  El modelo
         * unificado: ALLOCAs en VM-stack, host_ptrs en is_host_ptr=true.
         * El LOAD/STORE path emite:
         *   - host_in_jit=true (malloc/new/fields GC): native mov.
         *   - !host_in_jit (VM-addr de ALLOCA/param): inline page cache
         *     hit + fallback vrt_vm_read/write per-size.  */

        /* LICM para ALLOCA: las ALLOCAs en bloques no-entry causan
         * stack overflow si estan dentro de loops (cada iter consume
         * bytes sin liberarlos antes de la back-edge).  Solucion:
         * hoist a entry.  Cada slot stack se reusa entre iteraciones.
         *
         * Construimos un mapa: VID de ALLOCA -> bytes a reservar.  En
         * el prologue del MBlock entry emitimos los `sub rsp, N` para
         * todas las ALLOCAs hoisted Y guardamos el `mov dst_slot, rsp`
         * (ajustado al offset acumulado).  En el switch de IrOp::ALLOCA
         * dentro del loop body, si el VID esta en el mapa, NO emitimos
         * nada (ya esta hoisted). */
        std::unordered_map<ir::IrValueId, uint64_t> hoisted_allocas;
        std::vector<std::pair<ir::IrValueId, uint64_t>> hoisted_order;
        for (size_t bi_chk = 1; bi_chk < ir_fn.blocks.size(); ++bi_chk) {
            for (const auto &ins_chk : ir_fn.blocks[bi_chk].instrs) {
                if (ins_chk.op == ir::IrOp::ALLOCA
                 && ins_chk.dst != ir::IR_NO_VALUE) {
                    const uint64_t bytes = ins_chk.imm;
                    if (bytes == 0 || bytes >= INT32_MAX) continue;
                    const uint64_t aligned = (bytes + 15ULL) & ~15ULL;
                    hoisted_allocas[ins_chk.dst] = aligned;
                    hoisted_order.emplace_back(ins_chk.dst, aligned);
                }
            }
        }

        const JitRegalloc regalloc = compute_jit_regalloc(ir_fn);

        /* Sprint CCC: contar usos por VID para habilitar fusion CMP+BR_COND.
         * El selector emite `xor rax,rax; cmp; setcc; store` para CMP_* y
         * luego `load; test; jcc` para BR_COND -- 8 instr.  Con fusion:
         * `cmp; jcc<cond>` -- 2 instr.  Solo seguro cuando el resultado del
         * CMP_* tiene EXACTAMENTE un uso (el BR_COND).  use_count cubre
         * todos los operands + phi_args. */
        std::vector<uint32_t> use_count(ir_fn.values.size(), 0);
        for (const auto &blk : ir_fn.blocks) {
            for (const auto &ins : blk.instrs) {
                for (ir::IrValueId v : ins.operands) {
                    if (v != ir::IR_NO_VALUE && v < use_count.size()) use_count[v]++;
                }
                for (const auto &pa : ins.phi_args) {
                    if (pa.value != ir::IR_NO_VALUE && pa.value < use_count.size())
                        use_count[pa.value]++;
                }
                if (ins.func_ptr != ir::IR_NO_VALUE && ins.func_ptr < use_count.size())
                    use_count[ins.func_ptr]++;
            }
        }

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

        /* Phase D.jit-mem-model VM-STACK: reservamos 16 bytes extras al
         * tope del frame (justo bajo los slots SSA) para:
         *   - saved_vm_rsp: VM-RSP original al entry, restaurado al RET.
         *   - hoisted_base:  VM-RSP post-hoist, base de las ALLOCAs hoisted.
         * Offsets desde RBP (constantes durante toda la funcion).
         * Solo se usan si la funcion tiene ALLOCAs; el overhead es 16
         * bytes extra del frame en CADA funcion (insignificante). */
        const int32_t vm_rsp_save_off  = -static_cast<int32_t>(slot_bytes + 8);
        const int32_t hoisted_base_off = -static_cast<int32_t>(slot_bytes + 16);
        const uint32_t VM_STACK_SLOTS_BYTES = 16;

        /* ===== callback-ABI: analisis del prologo/epilogo nativo ===== */
        const bool cb_entry = opts_.callback_entry
                           && opts_.mode == SelectorMode::VM_ABI;
        /* argc del callback = numero de params (cap 12 = calling convention VM). */
        const uint32_t cb_argc = cb_entry
            ? static_cast<uint32_t>(ir_fn.params.size() > 12 ? 12 : ir_fn.params.size())
            : 0u;
        /* TLS-direct disponible? (-1 = usar el call fallback). */
        const bool cb_use_call = cb_entry && (opts_.callback_tls_gs_disp == -1);
        /* Numero de args que viajan en registros segun el ABI nativo. */
#if defined(_WIN32)
        const uint32_t CB_N_REG_ARGS = 4;
#else
        const uint32_t CB_N_REG_ARGS = 6;
#endif
        const uint32_t cb_n_reg_args = cb_entry
            ? (cb_argc < CB_N_REG_ARGS ? cb_argc : CB_N_REG_ARGS) : 0u;
        /* save-set: cuerpo hoja-puro -> no salvar nada; cualquier op no
         * hoja-segura -> salvar el banco completo R0..R15 (modo "safe",
         * equivalente al thunk previo). */
        bool cb_save_all = false;
        if (cb_entry) {
            for (const auto &blk : ir_fn.blocks) {
                for (const auto &ins : blk.instrs) {
                    if (!cb_is_leaf_safe_op(ins.op)) { cb_save_all = true; break; }
                }
                if (cb_save_all) break;
            }
        }
        /* Work area del callback, debajo de los VM-STACK slots:
         *   - save area de proc->regs[0..15] (128 B) si cb_save_all.
         *   - spill area de los reg-args (si cb_use_call). */
        const uint32_t cb_save_bytes  = (cb_entry && cb_save_all) ? 128u : 0u;
        const uint32_t cb_spill_bytes = (cb_entry && cb_use_call)
            ? ((cb_n_reg_args * 8u + 15u) & ~15u) : 0u;
        uint32_t cb_work_bytes = cb_save_bytes + cb_spill_bytes;
        if (cb_work_bytes & 15) cb_work_bytes = (cb_work_bytes + 15) & ~15u;
        /* Base de la work area (offset positivo desde RBP, se niega al usar). */
        const int32_t cb_work_base = static_cast<int32_t>(slot_bytes + 16);
        /* Offset del slot de save del VM reg r (0..15) desde RBP. */
        auto cb_save_off = [&](uint32_t r) -> int32_t {
            return -(cb_work_base + 8 + static_cast<int32_t>(r) * 8);
        };
        /* Offset del slot de spill del reg-arg nativo i desde RBP. */
        auto cb_spill_off = [&](uint32_t i) -> int32_t {
            return -(cb_work_base + static_cast<int32_t>(cb_save_bytes)
                     + 8 + static_cast<int32_t>(i) * 8);
        };

        /* Padding fijo para alinear rsp + shadow space (solo Win64). */
        constexpr uint32_t ALIGN_PAD = 8;
#if defined(_WIN32)
        constexpr uint32_t SHADOW_SPACE = 32;
#else
        constexpr uint32_t SHADOW_SPACE = 0;
#endif
        uint32_t frame_size = slot_bytes + VM_STACK_SLOTS_BYTES
                            + cb_work_bytes + ALIGN_PAD + SHADOW_SPACE;
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
            /* push rbx (preservar; RBX = ProcessVM* durante toda la fn) */
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RBX)));
            if (!cb_entry) {
                /* mov rbx, <proc_reg>: en VM_ABI el proc llega como arg0.
                 * En callback-ABI NO hay arg de proc; se carga mas abajo via
                 * LOAD_PROC (tras montar el frame, por si usa el fallback). */
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
        }

        /* Profiler MIPS counter para JIT.  Se emite per-bloque (no
         * solo prologue): cada bloque ejecutado contribuye N instrs
         * al counter, donde N = numero de IR instrs del bloque.  Asi
         * los loops contribuyen N * iter_count, dando MIPS reales.
         *
         * Patron emitido al inicio de cada MBlock (1+1 = 2 instrs):
         *
         *     mov rax, imm64  ; addr del counter (mismo en todo el metodo)
         *     add qword [rax], N_block  ; N = ir_block.instrs.size()
         *
         * Coste: 2 instr (~3 ns) per block entry, including loop iterations.
         * El overhead es pequeno comparado con el trabajo del bloque (cada
         * bloque suele tener >=5 instrs reales).  Para bloques de 1-2 instrs
         * (raros), el overhead es relativo pero absoluto despreciable. */
        /* La emision per-block se hace MAS ABAJO, en el inicio del loop
         * de blocks (linea ~547).  El prologue NO necesita counter porque
         * no representa work del programa Vex; solo setup. */

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

        /* ===== C2 tier-up: contador on-entry + check (HotSpot-style) =====
         *
         * Emitido aqui, justo tras montar el frame (rsp 16-alineado + shadow
         * space dentro de frame_size) y antes de cualquier ALLOCA (que moveria
         * rsp).  RBX = ProcessVM* ya esta cargado (VM_ABI, !cb_entry).  Patron:
         *
         *     mov rax, &counter
         *     add qword [rax], 1
         *     cmp qword [rax], threshold
         *     jne tier_skip                 ; != threshold -> seguir normal
         *     mov arg0, rbx                  ; proc
         *     mov arg1, imm64(fn_pc)
         *     mov rax, &handler
         *     call rax                       ; c2_tier_up(proc, fn_pc): recompila+swap
         *   tier_skip:
         *
         * El `jne` (==) hace que dispare EXACTAMENTE una vez: tras cruzar el
         * umbral el contador sigue creciendo y nunca vuelve a igualarlo.  El
         * codigo C2 resultante NO lleva contador (terminal).  El frame C1 en
         * curso termina en C1; las entradas futuras van al C2 via el swap.
         * Los regs caller-saved que clobbea el call son seguros: el cuerpo aun
         * no cargo ningun valor VM (vienen de proc->regs en memoria). */
        if (tier_counter_addr != 0) {
#if defined(_WIN32)
            const MReg TU_ARG0 = MReg::RCX;
            const MReg TU_ARG1 = MReg::RDX;
#else
            const MReg TU_ARG0 = MReg::RDI;
            const MReg TU_ARG1 = MReg::RSI;
#endif
            const MLabelId tier_skip = mf.new_label();
            const uint32_t cnt_idx = mf.intern_imm64(tier_counter_addr);
            /* mov rax, &counter */
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm64_idx(cnt_idx)));
            /* add qword [rax], 1 */
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::ADD,
                MOperand::make_mem(MReg::RAX, 0),
                MOperand::make_imm32(1)));
            /* cmp qword [rax], threshold */
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::CMP,
                MOperand::make_mem(MReg::RAX, 0),
                MOperand::make_imm32(static_cast<int32_t>(opts_.tier_up_threshold))));
            /* jne tier_skip */
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_jcc(MCond::NE, tier_skip));
            /* mov arg0, rbx (proc) */
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(TU_ARG0),
                MOperand::make_reg(MReg::RBX)));
            /* mov arg1, imm64(fn_pc) */
            const uint32_t pc_idx = mf.intern_imm64(ic_fn_pc);
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(TU_ARG1),
                MOperand::make_imm64_idx(pc_idx)));
            /* mov rax, &handler */
            const uint32_t h_idx = mf.intern_imm64(opts_.tier_up_handler_addr);
            mf.blocks[prologue].instrs.push_back(MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm64_idx(h_idx)));
            /* call rax */
            {
                MInstr tu_call;
                tu_call.op = MOp::CALL;
                tu_call.src1 = MOperand::make_reg(MReg::RAX);
                mf.blocks[prologue].instrs.push_back(tu_call);
            }
            /* tier_skip: */
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_label_def(tier_skip));
        }

        /* ===== callback-ABI: cargar ProcessVM* en RBX ===== */
        if (cb_entry) {
            /* Registros de args nativos en orden del ABI. */
#if defined(_WIN32)
            const MReg CB_ARG_REGS[] = {MReg::RCX, MReg::RDX, MReg::R8, MReg::R9};
#else
            const MReg CB_ARG_REGS[] = {MReg::RDI, MReg::RSI, MReg::RDX,
                                        MReg::RCX, MReg::R8, MReg::R9};
#endif
            /* Si usamos el call-fallback, el call de LOAD_PROC clobbeara los
             * registros de args nativos; spill-earlos AHORA (antes del call).
             * En TLS-direct LOAD_PROC es un solo gs-mov a RBX que no toca los
             * arg regs, asi que el spill no hace falta (se leen directo en el
             * param-setup). */
            if (cb_use_call) {
                for (uint32_t i = 0; i < cb_n_reg_args; ++i) {
                    mf.blocks[prologue].instrs.push_back(
                        MInstr::make_unary(MOp::MOV,
                            MOperand::make_mem(MReg::RBP, cb_spill_off(i)),
                            MOperand::make_reg(CB_ARG_REGS[i])));
                }
            }
            /* LOAD_PROC: RBX = ProcessVM* (TLS-direct o call fallback). */
            const uint32_t getproc_idx =
                mf.intern_imm64(opts_.callback_get_proc_addr);
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_load_proc(MReg::RBX,
                    opts_.callback_tls_gs_disp, getproc_idx));
        }

        /* Phase D.jit-mem-model VM-STACK: ALLOCAs ahora viven en VM-stack
         * (consistente con interp), no en host stack.  Eso permite:
         *   - Mixing JIT/interp seguro (mismo modelo de memoria).
         *   - Sin host stack overflow (i32[100000] etc. funcionan).
         *   - LOAD/STORE sobre ALLOCA-derived ptr va por inline page
         *     cache hit (~5 ns) o fallback vm_read/write.
         *
         * Detectamos si la fn tiene ALLOCAs (hoisted o no) para emitir
         * el save de VM-RSP solo cuando sea necesario (cero overhead
         * para fns sin ALLOCA, mayoria del codigo). */
        bool fn_has_alloca = !hoisted_order.empty();
        if (!fn_has_alloca && opts_.mode == SelectorMode::VM_ABI) {
            for (const auto &blk : ir_fn.blocks) {
                for (const auto &ins_chk : blk.instrs) {
                    if (ins_chk.op == ir::IrOp::ALLOCA) {
                        fn_has_alloca = true;
                        break;
                    }
                }
                if (fn_has_alloca) break;
            }
        }

        /* Save VM-RSP original al saved_vm_rsp slot.  Cualquier ALLOCA
         * mas adelante modificara VM-RSP; el RET restaurara desde aqui. */
        if (fn_has_alloca && opts_.mode == SelectorMode::VM_ABI) {
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_reg(SCRATCH_A),
                    MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET)));
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_mem(MReg::RBP, vm_rsp_save_off),
                    MOperand::make_reg(SCRATCH_A)));
        }

        /* LICM ALLOCA hoist en VM-stack: reservar todas las ALLOCAs de
         * bloques no-entry al inicio del prologue, restando una sola vez
         * de VM-RSP.  Cada ALLOCA hoisted retorna hoisted_base + offset_fijo.
         *
         * Mapa: VID -> offset POSITIVO desde hoisted_base (= VM-RSP
         * post-hoist).  Computamos offsets tras conocer total. */
        std::unordered_map<ir::IrValueId, int32_t> hoisted_vm_offset;
        uint64_t hoisted_total = 0;
        if (!hoisted_order.empty() && opts_.mode == SelectorMode::VM_ABI) {
            uint64_t accum = 0;
            for (const auto &kv : hoisted_order) accum += kv.second;
            hoisted_total = accum;
            /* offset desde hoisted_base = total - accum_runup (orden de
             * hoist en hoisted_order da la pila descendente del VM). */
            uint64_t accum_runup = 0;
            for (const auto &kv : hoisted_order) {
                accum_runup += kv.second;
                const int32_t off = static_cast<int32_t>(hoisted_total - accum_runup);
                hoisted_vm_offset[kv.first] = off;
            }
            /* SCRATCH_A ya tiene VM-RSP original (cargado arriba).
             *   sub rax, hoisted_total
             *   mov [rbx + STACK_POINTER_OFFSET], rax
             *   mov [rbp + hoisted_base_off], rax
             * Despues cada ALLOCA hoisted reads [rbp + hoisted_base_off]
             * + add fixed_offset. */
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::SUB,
                    MOperand::make_reg(SCRATCH_A),
                    MOperand::make_imm32(static_cast<int32_t>(hoisted_total))));
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                    MOperand::make_reg(SCRATCH_A)));
            mf.blocks[prologue].instrs.push_back(
                MInstr::make_unary(MOp::MOV,
                    MOperand::make_mem(MReg::RBP, hoisted_base_off),
                    MOperand::make_reg(SCRATCH_A)));
        }

        /* Map dummy para el codigo viejo que iteraba hoisted_rbp_offset.
         * Lo reescribiremos abajo en el case ALLOCA usando hoisted_vm_offset. */
        std::unordered_map<ir::IrValueId, int32_t> hoisted_rbp_offset; /* unused, kept for ABI */

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
        } else if (cb_entry) {
            /* ===== callback-ABI: mover args nativos a los homes de params =====
             *
             * Los args llegan por la convencion C nativa (regs + stack), NO en
             * proc->registers.  Los movemos directo al home de cada param
             * (slot stack o vreg, resuelto por el regalloc rewrite).
             *
             * En modo SAFE (cuerpo con CALL/raw_asm/etc.) ademas:
             *   - salvamos proc->registers[0..15] al save area ANTES de
             *     pisarlos (re-entrancia del callback);
             *   - replicamos cada arg en proc->registers[R1+i] (para que
             *     READ_VM_REG y el marshalling de CALLs vean los args);
             *   - seteamos proc->registers[R15] = argc.
             * En modo FAST (hoja pura) NO se toca proc->registers en absoluto. */
#if defined(_WIN32)
            const MReg CB_ARG_REGS[] = {MReg::RCX, MReg::RDX, MReg::R8, MReg::R9};
#else
            const MReg CB_ARG_REGS[] = {MReg::RDI, MReg::RSI, MReg::RDX,
                                        MReg::RCX, MReg::R8, MReg::R9};
#endif
            const int32_t N_cs = static_cast<int32_t>(regalloc.callee_saved_used.size());
            /* Offset (desde RBP) del arg nativo i cuando viaja en el stack del
             * caller (i >= CB_N_REG_ARGS).  Deriva del layout de pushes:
             *   [rbp], CS[N_cs-1..0], rbx, retaddr, [shadow Win64], stack-args. */
            auto cb_caller_arg_off = [&](uint32_t i) -> int32_t {
#if defined(_WIN32)
                return 56 + 8 * N_cs + (static_cast<int32_t>(i) - 4) * 8;
#else
                return 24 + 8 * N_cs + (static_cast<int32_t>(i) - 6) * 8;
#endif
            };
            /* Carga el arg i en SCRATCH_A (rax). */
            auto cb_load_arg_to_rax = [&](uint32_t i) {
                if (i < cb_n_reg_args) {
                    if (cb_use_call) {
                        mf.blocks[prologue].instrs.push_back(
                            MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_A),
                                MOperand::make_mem(MReg::RBP, cb_spill_off(i))));
                    } else {
                        mf.blocks[prologue].instrs.push_back(
                            MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(CB_ARG_REGS[i])));
                    }
                } else {
                    mf.blocks[prologue].instrs.push_back(
                        MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_A),
                            MOperand::make_mem(MReg::RBP, cb_caller_arg_off(i))));
                }
            };

            /* SAFE: salvar proc->registers[0..15] al save area (rax scratch). */
            if (cb_save_all) {
                for (uint32_t r = 0; r < 16; ++r) {
                    const int32_t reg_off =
                        VESTA_PROC_REGISTERS_OFFSET + static_cast<int32_t>(r) * VESTA_REGISTER_SIZE;
                    mf.blocks[prologue].instrs.push_back(
                        MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_A),
                            MOperand::make_mem(MReg::RBX, reg_off)));
                    mf.blocks[prologue].instrs.push_back(
                        MInstr::make_unary(MOp::MOV,
                            MOperand::make_mem(MReg::RBP, cb_save_off(r)),
                            MOperand::make_reg(SCRATCH_A)));
                }
            }

            /* Mover args -> homes de params (+ proc->regs en modo safe). */
            for (uint32_t i = 0; i < cb_argc; ++i) {
                cb_load_arg_to_rax(i);
                if (cb_save_all) {
                    const int32_t reg_off = VESTA_PROC_REGISTERS_OFFSET
                        + static_cast<int32_t>(i + 1) * VESTA_REGISTER_SIZE;
                    mf.blocks[prologue].instrs.push_back(
                        MInstr::make_unary(MOp::MOV,
                            MOperand::make_mem(MReg::RBX, reg_off),
                            MOperand::make_reg(SCRATCH_A)));
                }
                store_op(mf, ir_fn.params[i], SCRATCH_A);
                mark_slot_as_gc_if_needed(ir_fn.params[i], ir::IrType::VOID);
            }

            /* SAFE: proc->registers[R15] = argc. */
            if (cb_save_all) {
                mf.blocks[prologue].instrs.push_back(
                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_A),
                        MOperand::make_imm32(static_cast<int32_t>(cb_argc))));
                const int32_t r15_off =
                    VESTA_PROC_REGISTERS_OFFSET + 15 * VESTA_REGISTER_SIZE;
                mf.blocks[prologue].instrs.push_back(
                    MInstr::make_unary(MOp::MOV,
                        MOperand::make_mem(MReg::RBX, r15_off),
                        MOperand::make_reg(SCRATCH_A)));
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

        /* Jump al primer IR block (entry).
         *
         * Optimizacion: el bloque entry (block_labels[0]) se crea SIEMPRE
         * justo despues del prologo (es @c mf.blocks[prologue+1]), asi que el
         * encoder lo emite contiguo -> el salto es un fall-through y el
         * `jmp` es redundante (5 bytes + 1 branch por invocacion).  Lo
         * omitimos; el prologo cae directo al primer bloque.  El label del
         * entry sigue definido, asi que cualquier back-edge que salte a el
         * (loop header) sigue resolviendo.  Aplica a todos los modos. */
        (void)block_labels;

        /* Lower cada IR block. */
        for (size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
            const auto &ir_block = ir_fn.blocks[bi];
            MBlockId mb = mf.new_block(block_labels[bi]);

            /* JIT MIPS counter per-block entry.  Cada vez que ejecuta este
             * bloque (incluyendo loop iterations), suma N al counter.
             * N = numero de IR instrs del bloque.  El registro counter_addr
             * se reusa entre bloques pero hay que reemitir el `mov rax` en
             * cada uno porque RAX puede haber sido clobbeado por las instrs
             * del bloque anterior.  Coste: 2 instr per block (~3 ns). */
            if (opts_.jit_instr_counter_addr != 0 && !ir_block.instrs.empty()) {
                const uint32_t cnt_idx = mf.intern_imm64(opts_.jit_instr_counter_addr);
                mf.blocks[mb].instrs.push_back(
                    MInstr::make_unary(MOp::MOV,
                        MOperand::make_reg(MReg::RAX),
                        MOperand::make_imm64_idx(cnt_idx)));
                const int32_t n_instrs = static_cast<int32_t>(
                    std::min(ir_block.instrs.size(), static_cast<size_t>(INT32_MAX)));
                mf.blocks[mb].instrs.push_back(
                    MInstr::make_unary(MOp::ADD,
                        MOperand::make_mem(MReg::RAX, 0),
                        MOperand::make_imm32(n_instrs)));
            }

            /* Estado de fusion CMP+BR_COND por bloque.  Reseteo al inicio.
             * Sprint CCC: el cmp se EMITE en el BR_COND (no en el CMP_*)
             * para que el safepoint de back-edge no clobree flags. */
            bool          fused_cmp_active = false;
            MCond         fused_cmp_cond   = MCond::E;
            ir::IrValueId fused_cmp_dst    = ir::IR_NO_VALUE;
            ir::IrValueId fused_cmp_op0    = ir::IR_NO_VALUE;
            ir::IrValueId fused_cmp_op1    = ir::IR_NO_VALUE;

            for (size_t ins_idx = 0; ins_idx < ir_block.instrs.size(); ++ins_idx) {
                const auto &ins = ir_block.instrs[ins_idx];
                using IrOp = ir::IrOp;
                switch (ins.op) {
                    /* --------- Mov / Const --------- */
                    case IrOp::NOP: break;
                    /* Markers semanticos para C2 (Phase D.8): no producen
                     * codigo, solo metadata.  La construccion real (ALLOCA
                     * + STOREs) la emite el lowering DESPUES del marker. */
                    case IrOp::MAKE_CLOSURE: break;
                    case IrOp::MAKE_VARIANT: break;
                    case IrOp::MATCH_VARIANT: break;
                    case IrOp::PHI: {
                        /* D.3-A: PHI es no-op en el successor.  Los copies
                         * para asignar el valor correcto al slot del dst
                         * ya fueron emitidos en cada predecesor antes de
                         * su BR/BR_COND. */
                        break;
                    }
                    case IrOp::MOV: {
                        if (ins.operands.empty()) break;
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
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
                    case IrOp::LABEL_ADDR: {
                        /* %dst = direccion VM del label `code.<func_name>`
                         * resuelta por el linker.  Equivalente al codigo
                         * emitido por el frontend interp:
                         *   `mov rDst, @Absolute("code.<func_name>")`.
                         *
                         * Sprint JIT-cross-fn 2026-06-01: necesario para
                         * `as_native_callback(fn)` builtin (B.1) y para
                         * cualquier funcion que necesite el PC virtual
                         * de otra fn Vex como valor (passing fn por valor,
                         * trampolines, registro de handlers, etc.).
                         *
                         * El nombre del label vive en @c ins.func_name.
                         * Lo resolvemos via @c opts_.resolve_symbol con
                         * el prefijo `code.` igual que el bytecode emit. */
                        if (ins.func_name.empty()) {
                            if (jit::g_jit_warn_unsupported) {
                                std::fprintf(stderr,
                                    "[jit] selector: LABEL_ADDR sin func_name en fn '%s' linea %u\n",
                                    ir_fn.name.c_str(), ins.source_line);
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const std::string sym = "code." + ins.func_name;
                        uint64_t addr = 0;
                        if (opts_.resolve_symbol) {
                            addr = opts_.resolve_symbol(sym);
                        }
                        if (addr == 0) {
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: LABEL_ADDR sin symbol '%s' en fn '%s' linea %u\n",
                                        sym.c_str(), ir_fn.name.c_str(), ins.source_line);
                                }
                            }
                            unsupported = true;
                            mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* mov rax, addr (via imm64 pool).  Asi el JIT
                         * emite secuencia de 10 bytes:
                         *   48 B8 <imm64>  ;  mov rax, imm64
                         * Y guardamos rax al slot del dst. */
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
                        const int64_t cv = static_cast<int64_t>(ins.imm);
                        if (cv >= -0x80000000LL && cv <= 0x7FFFFFFFLL) {
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
                                load_op_rematerializable(mf, ir_fn, op0, SCRATCH_A);
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
                                    load_op_rematerializable(mf, ir_fn, op1, SCRATCH_A);
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
                            load_op_rematerializable(mf, ir_fn, op0, SCRATCH_A);
                            load_op_rematerializable(mf, ir_fn, op1, SCRATCH_B);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::NEG,
                                MOperand::make_reg(SCRATCH_A), {}));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case IrOp::NOT: {
                        if (ins.operands.empty()) break;
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], MReg::RCX);
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

                    /* --------- ALLOCA (Phase D.jit-mem-model VM-STACK) ---
                     *
                     * Las ALLOCAs viven en VM-stack (mismo modelo que
                     * interp).  Asi:
                     *   - El IR is_host_ptr=false del ALLOCA dst es
                     *     consistente con el runtime real (VM-addr).
                     *   - Mixing JIT/interp es seguro.
                     *   - Arrays grandes (i32[100000]) no causan host
                     *     stack overflow.
                     *
                     * Hoisted: addr = [rbp + hoisted_base_off] + offset_fijo
                     *          (offset positivo precomputado en prologue).
                     * Non-hoisted (entry):
                     *          mov rax, [rbx + STACK_POINTER_OFFSET]
                     *          sub rax, aligned_size
                     *          mov [rbx + STACK_POINTER_OFFSET], rax
                     *          mov [slot_dst], rax */
                    case IrOp::ALLOCA: {
                        if (opts_.mode != SelectorMode::VM_ABI) {
                            /* NATIVE_ABI (tests sintieticos): mantener
                             * host stack para compatibilidad. */
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
                            store_op(mf, ins.dst, MReg::RSP);
                            break;
                        }

                        /* Phase D.jit-mem-model AUTO-PROMOTE: si el ALLOCA
                         * esta marcado `host_alloca=true` (por el IR pass
                         * ir_pass_promote_callned_allocas), emit host stack
                         * (sub rsp, N) -- el ptr resultante es directamente
                         * dereferenciable por code C nativo.  Auto-liberado
                         * por epilogue del JIT (mov rsp, rbp).
                         *
                         * Esto permite `&local` -> CALLN natural C-style:
                         *   u8[1024] buf;
                         *   ReadFile(h, &buf[0], 1024, ...);
                         *
                         * Sprint mem-loop-fix (2026-06-02): EXCEPCION cuando
                         * `host_alloca_explicit_free=true`.  El IR pass
                         * `ir_pass_promote_local_raw_alloc` marca asi a
                         * ALLOCAs que tienen un RAW_FREE explicito
                         * preservado (caso `malloc(N)+...+free(p)` dentro
                         * de un loop).  Si emitiesemos `sub rsp, N` para
                         * cada iter, el host stack crece 96 bytes/iter
                         * sin liberar (no hay `add rsp` correspondiente)
                         * -> 5M iter * 96 = 480 MB stack exhaustion ->
                         * SEGV.  En este caso, caer al path slab
                         * (CALL vrt_raw_alloc + el RAW_FREE preservado
                         * llama vrt_raw_free correctamente).  */
                        /* Sprint mem-loop-fix-v2: AMBOS casos
                         * (con/sin explicit_free) emiten `sub rsp, N`.
                         * El RAW_FREE matching emite `add rsp, N` para
                         * balancear el stack per-iteracion (ver case
                         * RAW_FREE).  Asi loops con malloc/free dentro
                         * de cuerpos inlineados no acumulan stack. */
                        if (ins.host_alloca) {
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
                            store_op(mf, ins.dst, MReg::RSP);
                            break;
                        }

                        /* Hoisted: la reserva ya se hizo en el prologue;
                         * solo computar el ptr dentro del bloque hoisted. */
                        auto hv_it = hoisted_vm_offset.find(ins.dst);
                        if (hv_it != hoisted_vm_offset.end()) {
                            const int32_t off_in_block = hv_it->second;
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_mem(MReg::RBP, hoisted_base_off)));
                            if (off_in_block != 0) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(off_in_block)));
                            }
                            store_op(mf, ins.dst, SCRATCH_A);
                            break;
                        }

                        /* Non-hoisted (entry block).  Reservar en VM-stack
                         * via proc->stack_pointer. */
                        const uint64_t size_bytes = ins.imm;
                        const uint64_t aligned =
                            (size_bytes > 0 && size_bytes < INT32_MAX)
                                ? ((size_bytes + 15ULL) & ~15ULL)
                                : 0ULL;
                        /* mov rax, [rbx + STACK_POINTER_OFFSET] */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET)));
                        if (aligned > 0) {
                            /* sub rax, aligned */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::SUB,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_imm32(static_cast<int32_t>(aligned))));
                            /* mov [rbx + STACK_POINTER_OFFSET], rax */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                                    MOperand::make_reg(SCRATCH_A)));
                        }
                        store_op(mf, ins.dst, SCRATCH_A);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);

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
                        const ir::IrValueId p_vid = ins.operands[0];
                        const uint64_t lbytes = ir_type_size_bytes(ins.type);
                        const bool lsigned = ir_type_is_signed_int(ins.type);
                        const bool ptr_is_host = (p_vid < host_in_jit.size()
                                              && host_in_jit[p_vid]);
                        load_op_rematerializable(mf, ir_fn, p_vid, SCRATCH_B);

                        if (!ptr_is_host
                         && opts_.mode == SelectorMode::VM_ABI
                         && opts_.runtime != nullptr) {
                            /* Phase D.jit-mem-model INLINE-CACHE.
                             *
                             * Para VM-addr (is_host_ptr=false en IR),
                             * primero intentamos inline page cache hit:
                             *
                             *   mov rax, rcx              ; rax = vaddr
                             *   and rax, -4096            ; page-align
                             *   cmp rax, [rbx + page_v]  ; cached_page_vaddr
                             *   jne miss
                             *   mov rdx, rcx
                             *   and rdx, 4095            ; offset en pagina
                             *   cmp rdx, 4096-size       ; cross-page?
                             *   ja  miss
                             *   mov rax, [rbx + page_h]  ; cached_page_host
                             *   add rax, rdx
                             *   ; native MOV/MOVSX/MOVZX desde [rax]
                             *   jmp done
                             *  miss:
                             *   ; call vrt_vm_read_u<size>
                             *  done:
                             *
                             * Hit (95% accesos secuenciales): ~5 ns.
                             * Miss/cross-page: ~30 ns (call al runtime).
                             * Para size=1 (no puede cruzar), skip el
                             * cross-page check (3 instrs menos en hot). */
                            uint64_t fn_addr = 0;
                            switch (lbytes) {
                                case 1: fn_addr = (uint64_t)opts_.runtime->vm_read_u8;  break;
                                case 2: fn_addr = (uint64_t)opts_.runtime->vm_read_u16; break;
                                case 4: fn_addr = (uint64_t)opts_.runtime->vm_read_u32; break;
                                default: fn_addr = (uint64_t)opts_.runtime->vm_read_u64; break;
                            }
                            if (fn_addr == 0) {
                                if (jit::g_jit_warn_unsupported) {
                                    std::fprintf(stderr,
                                        "[jit] selector: LOAD VM-ptr sin runtime entry para size %llu en fn '%s' linea %u\n",
                                        (unsigned long long)lbytes,
                                        ir_fn.name.c_str(), ins.source_line);
                                }
                                if (out_unsupported) *out_unsupported = true;
                                return mf;
                            }

                            /* Phase D.jit-mem-model INLINE-CACHE activado.
                             *
                             * Inline page cache hit: si la pagina del vaddr
                             * coincide con cached_page_vaddr y no cruza
                             * boundary, native mov directo (~5 ns).
                             * Si miss/cross-page, fallback runtime call.
                             *
                             * Patron x86-64:
                             *   mov rax, rcx           ; rcx = vaddr
                             *   and rax, -4096
                             *   cmp rax, [rbx + page_v_disp]
                             *   jne miss
                             *   mov rdx, rcx           ; offset = vaddr & 0xFFF
                             *   and rdx, 4095
                             *   [cmp rdx, 4096-size; ja miss  ; cross-page check]
                             *   mov rax, [rbx + page_h_disp]
                             *   add rax, rdx
                             *   native MOV/MOVSX/MOVZX
                             *   jmp done
                             *  miss:
                             *   ; runtime call vrt_vm_read_u<size>
                             *  done: */
                            const bool inline_cache_ok =
                                vesta_rt::kProcVmMemOffset != 0
                             && vesta_rt::kVmMemCachedPageVaddrOffset >= 0
                             && vesta_rt::kVmMemCachedPageHostOffset >= 0;
                            const MLabelId miss_label =
                                inline_cache_ok ? mf.new_label() : MLABEL_INVALID;
                            const MLabelId done_label =
                                inline_cache_ok ? mf.new_label() : MLABEL_INVALID;

                            if (inline_cache_ok) {
                                const int32_t page_v_disp =
                                    vesta_rt::kProcVmMemOffset + vesta_rt::kVmMemCachedPageVaddrOffset;
                                const int32_t page_h_disp =
                                    vesta_rt::kProcVmMemOffset + vesta_rt::kVmMemCachedPageHostOffset;

                                /* SCRATCH_B (RCX) tiene vaddr tras el
                                 * load_op anterior.  Copiamos a SCRATCH_C
                                 * (RDX) directamente con un MOV reg-reg
                                 * (que NO es optimizable por slot peephole).
                                 * Asi tenemos vaddr en ambos para usar
                                 * en page-align (RAX) y offset (RDX). */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_reg(SCRATCH_B)));
                                /* mov rax, rdx  (page-align candidate) */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_C)));
                                /* and rax, -4096  (page-aligned vaddr) */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::AND,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(-4096)));
                                /* cmp rax, [rbx + page_v_disp] */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::CMP,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_mem(JIT_PROC_REG, page_v_disp)));
                                /* jne miss */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(MCond::NE, miss_label));
                                /* Recompute offset within page sobre RDX
                                 * (que tiene vaddr).  Tras este AND, RDX
                                 * tiene offset (perdimos vaddr). */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::AND,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_imm32(4095)));
                                /* Cross-page check si size > 1. */
                                if (lbytes > 1) {
                                    const int32_t max_off =
                                        4096 - static_cast<int32_t>(lbytes);
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::CMP,
                                            MOperand::make_reg(SCRATCH_C),
                                            MOperand::make_imm32(max_off)));
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_jcc(MCond::A, miss_label));
                                }
                                /* mov rax, [rbx + page_h_disp]  ; cached_host */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_mem(JIT_PROC_REG, page_h_disp)));
                                /* add rax, rdx  ; host_ptr = host_base + offset */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_C)));
                                /* native LOAD desde [SCRATCH_A] segun size/sign. */
                                if (lbytes == 0 || lbytes >= 8) {
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(SCRATCH_A, 8),
                                            MOperand::make_mem(SCRATCH_A, 0)));
                                } else if (lsigned) {
                                    MOperand mem_op = MOperand::make_mem(SCRATCH_A, 0);
                                    mem_op.flags = static_cast<uint8_t>(lbytes);
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOVSX,
                                            MOperand::make_reg(SCRATCH_A, 8),
                                            mem_op));
                                } else if (lbytes == 4) {
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(SCRATCH_A, 4),
                                            MOperand::make_mem(SCRATCH_A, 0)));
                                } else {
                                    MOperand mem_op = MOperand::make_mem(SCRATCH_A, 0);
                                    mem_op.flags = static_cast<uint8_t>(lbytes);
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOVZX,
                                            MOperand::make_reg(SCRATCH_A, 8),
                                            mem_op));
                                }
                                /* jmp done -- el store_op comun al final
                                 * tras done_label escribira SCRATCH_A. */
                                mf.blocks.back().instrs.push_back(MInstr::make_jmp(done_label));
                                /* miss: */
                                mf.blocks.back().instrs.push_back(MInstr::make_label_def(miss_label));
                            }

                            /* Fallback: call vrt_vm_read_u<size>(proc, vaddr).
                             * Re-cargar vaddr DIRECTAMENTE desde el slot a
                             * NATIVE_ARG1 (RDX en Windows) -- no asumimos
                             * que SCRATCH_B tenga vaddr porque el inline
                             * cache hit lo machaco. */
                            load_op_rematerializable(mf, ir_fn, p_vid, NATIVE_ARG1);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(NATIVE_ARG0),
                                    MOperand::make_reg(JIT_PROC_REG)));
                            const uint32_t fn_pool_idx_l = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_imm64_idx(fn_pool_idx_l)));
                            MInstr call_ins{};
                            call_ins.op = MOp::CALL;
                            call_ins.src1 = MOperand::make_reg(SCRATCH_A);
                            mf.blocks.back().instrs.push_back(call_ins);
                            if (lbytes < 8 && lsigned) {
                                MOperand src_reg = MOperand::make_reg(SCRATCH_A);
                                src_reg.width = static_cast<uint8_t>(lbytes);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOVSX,
                                        MOperand::make_reg(SCRATCH_A, 8),
                                        src_reg));
                            }
                            if (inline_cache_ok) {
                                /* done: */
                                mf.blocks.back().instrs.push_back(MInstr::make_label_def(done_label));
                            }
                            store_op(mf, ins.dst, SCRATCH_A);
                            break;
                        }

                        /* host_ptr path: native mov directo. */
                        if (lbytes == 0 || lbytes >= 8) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A, 8),
                                    MOperand::make_mem(SCRATCH_B, 0)));
                        } else if (lsigned) {
                            MOperand mem_op = MOperand::make_mem(SCRATCH_B, 0);
                            mem_op.flags = static_cast<uint8_t>(lbytes);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOVSX,
                                    MOperand::make_reg(SCRATCH_A, 8),
                                    mem_op));
                        } else if (lbytes == 4) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A, 4),
                                    MOperand::make_mem(SCRATCH_B, 0)));
                        } else {
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
                        const ir::IrValueId ptr_vid = ins.operands[1];
                        const bool ptr_is_host = (ptr_vid < host_in_jit.size()
                                               && host_in_jit[ptr_vid]);
                        const uint64_t sbytes = ir_type_size_bytes(ins.type);

                        /* Phase D.jit-mem-model INLINE-CACHE.  Mismo
                         * patron que LOAD: hit en page cache -> native
                         * mov; miss/cross-page -> call al runtime. */
                        if (!ptr_is_host
                         && opts_.mode == SelectorMode::VM_ABI
                         && opts_.runtime != nullptr) {
                            uint64_t fn_addr = 0;
                            switch (sbytes) {
                                case 1: fn_addr = (uint64_t)opts_.runtime->vm_write_u8;  break;
                                case 2: fn_addr = (uint64_t)opts_.runtime->vm_write_u16; break;
                                case 4: fn_addr = (uint64_t)opts_.runtime->vm_write_u32; break;
                                default: fn_addr = (uint64_t)opts_.runtime->vm_write_u64; break;
                            }
                            if (fn_addr == 0) {
                                if (jit::g_jit_warn_unsupported) {
                                    std::fprintf(stderr,
                                        "[jit] selector: STORE VM-ptr sin runtime entry para size %llu en fn '%s' linea %u\n",
                                        (unsigned long long)sbytes,
                                        ir_fn.name.c_str(), ins.source_line);
                                }
                                if (out_unsupported) *out_unsupported = true;
                                return mf;
                            }

                            /* Phase D.jit-mem-model INLINE-CACHE STORE.
                             * Mismo patron que LOAD pero escribiendo. */
                            const bool inline_cache_ok =
                                vesta_rt::kProcVmMemOffset != 0
                             && vesta_rt::kVmMemCachedPageVaddrOffset >= 0
                             && vesta_rt::kVmMemCachedPageHostOffset >= 0;
                            const MLabelId miss_label =
                                inline_cache_ok ? mf.new_label() : MLABEL_INVALID;
                            const MLabelId done_label =
                                inline_cache_ok ? mf.new_label() : MLABEL_INVALID;

                            if (inline_cache_ok) {
                                const int32_t page_v_disp =
                                    vesta_rt::kProcVmMemOffset + vesta_rt::kVmMemCachedPageVaddrOffset;
                                const int32_t page_h_disp =
                                    vesta_rt::kProcVmMemOffset + vesta_rt::kVmMemCachedPageHostOffset;

                                /* Cargar vaddr a SCRATCH_B (RCX) primero
                                 * (load_op) y copiar a SCRATCH_C (RDX)
                                 * con MOV reg-reg directo (no optimizable). */
                                load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_reg(SCRATCH_B)));
                                /* mov rax, rdx; and rax, -4096 */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_C)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::AND,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_imm32(-4096)));
                                /* cmp rax, [rbx + page_v_disp]; jne miss */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::CMP,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_mem(JIT_PROC_REG, page_v_disp)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(MCond::NE, miss_label));
                                /* and rdx, 4095  (offset within page) */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::AND,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_imm32(4095)));
                                if (sbytes > 1) {
                                    const int32_t max_off =
                                        4096 - static_cast<int32_t>(sbytes);
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::CMP,
                                            MOperand::make_reg(SCRATCH_C),
                                            MOperand::make_imm32(max_off)));
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_jcc(MCond::A, miss_label));
                                }
                                /* host_ptr = cached_host + offset -> SCRATCH_B */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_B),
                                        MOperand::make_mem(JIT_PROC_REG, page_h_disp)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(SCRATCH_B),
                                        MOperand::make_reg(SCRATCH_C)));
                                /* load value -> SCRATCH_A + native store */
                                load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                                const uint8_t hw = (sbytes == 1 || sbytes == 2
                                                 || sbytes == 4 || sbytes == 8)
                                                 ? static_cast<uint8_t>(sbytes) : 8;
                                MOperand src_h = MOperand::make_reg(SCRATCH_A, hw);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(SCRATCH_B, 0),
                                        src_h));
                                mf.blocks.back().instrs.push_back(MInstr::make_jmp(done_label));
                                /* miss: */
                                mf.blocks.back().instrs.push_back(MInstr::make_label_def(miss_label));
                            }

                            /* Fallback: call vrt_vm_write_u<size>(proc, vaddr, value).
                             * Cargar en orden vaddr, value, proc para evitar
                             * clobber. */
                            load_op_rematerializable(mf, ir_fn, ins.operands[1], NATIVE_ARG1); /* vaddr */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], NATIVE_ARG2); /* value */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(NATIVE_ARG0),
                                    MOperand::make_reg(JIT_PROC_REG)));
                            const uint32_t fn_pool_idx_s = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_imm64_idx(fn_pool_idx_s)));
                            MInstr call_ins{};
                            call_ins.op = MOp::CALL;
                            call_ins.src1 = MOperand::make_reg(SCRATCH_A);
                            mf.blocks.back().instrs.push_back(call_ins);
                            if (inline_cache_ok) {
                                mf.blocks.back().instrs.push_back(MInstr::make_label_def(done_label));
                            }
                            break;
                        }

                        /* host_ptr STORE: native mov. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
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

                        /* Sprint CCC: deteccion de fusion CMP+BR_COND.
                         * Si la siguiente IR instr es BR_COND con
                         * operands[0] == este dst Y el dst solo tiene UN uso
                         * (este BR_COND), emitimos solo CMP y dejamos la
                         * condicion en flags.  El BR_COND handler emite jcc
                         * directo sin re-test.  Ahorra ~6 instr/iter en
                         * loops aritmeticos. */
                        bool fuse = false;
                        if (ins.dst != ir::IR_NO_VALUE
                         && ins.dst < use_count.size()
                         && use_count[ins.dst] == 1
                         && ins_idx + 1 < ir_block.instrs.size()) {
                            const auto &nxt = ir_block.instrs[ins_idx + 1];
                            if (nxt.op == IrOp::BR_COND
                             && !nxt.operands.empty()
                             && nxt.operands[0] == ins.dst) {
                                fuse = true;
                            }
                        }

                        if (fuse) {
                            /* NO emitimos cmp aqui; lo emitiremos en el
                             * BR_COND DESPUES del safepoint poll para que
                             * el handler no clobree las flags. */
                            fused_cmp_active = true;
                            fused_cmp_cond   = cond_for_cmp_op(ins.op);
                            fused_cmp_dst    = ins.dst;
                            fused_cmp_op0    = ins.operands[0];
                            fused_cmp_op1    = ins.operands[1];
                            break;
                        }

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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_C);
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
                    /* Sprint fib-recursion (2026-06-02): TAILCALL se trata
                     * como CALL normal + RET implicito.  Pierde la
                     * optimizacion de "no growing call stack" del TCO
                     * verdadero, pero permite compilar `main` cuando el
                     * IR optimizer convierte `return fn(args)` en
                     * tailcall.  El JIT no puede emitir el verdadero TCO
                     * (jump en lugar de call) sin coordinacion con el
                     * regalloc + frame cleanup.  Trade-off aceptado en
                     * v1: paridad con interp (que tambien lo trata como
                     * CALL+RET cuando no hay loop oportunista). */
                    case IrOp::TAILCALL:
                    case IrOp::CALL: {
                        /* Sprint fib-recursion: helper para emitir epilogue
                         * tras TAILCALL (= CALL + RET).  RAX ya contiene
                         * el return value (cargado en cada path).  Tambien
                         * lo escribe a proc->regs[0] para que el caller
                         * bytecode lo vea, y emite el cleanup del frame
                         * + ret host. */
                        const bool is_tailcall = (ins.op == IrOp::TAILCALL);
                        auto emit_tailcall_epilogue = [&]() {
                            if (!is_tailcall) return;
                            /* Cargar return value en RAX desde proc->regs[0]
                             * (donde el callee lo escribio) y propagarlo. */
                            if (opts_.mode == SelectorMode::VM_ABI) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_mem(MReg::RBX,
                                            VESTA_PROC_REGISTERS_OFFSET)));
                                /* callback-ABI safe: restaurar el banco VM
                                 * salvado en el prologo (re-entrancia).  RAX
                                 * (return) intacto; usamos SCRATCH_B. */
                                if (cb_entry && cb_save_all) {
                                    for (uint32_t r = 0; r < 16; ++r) {
                                        const int32_t reg_off = VESTA_PROC_REGISTERS_OFFSET
                                            + static_cast<int32_t>(r) * VESTA_REGISTER_SIZE;
                                        mf.blocks.back().instrs.push_back(
                                            MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_B),
                                                MOperand::make_mem(MReg::RBP, cb_save_off(r))));
                                        mf.blocks.back().instrs.push_back(
                                            MInstr::make_unary(MOp::MOV,
                                                MOperand::make_mem(MReg::RBX, reg_off),
                                                MOperand::make_reg(SCRATCH_B)));
                                    }
                                }
                                /* Restaurar VM-RSP si modificamos vm_stack. */
                                if (fn_has_alloca) {
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(SCRATCH_B),
                                            MOperand::make_mem(MReg::RBP, vm_rsp_save_off)));
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV,
                                            MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                                            MOperand::make_reg(SCRATCH_B)));
                                }
                            }
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RSP),
                                    MOperand::make_reg(MReg::RBP)));
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::POP,
                                    MOperand::make_reg(MReg::RBP), {}));
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
                        };

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

                        /* Sprint fib-recursion (2026-06-02): self-recursive
                         * call.  El resolver devuelve 0 con EAGER_IN_PROGRESS
                         * porque la funcion aun esta compilando.  Marcamos
                         * el imm64 como self-ref + usamos sentinel 0; el
                         * JitCompiler parchea con code_start tras la
                         * asignacion final.  Esto evita el trampoline
                         * JIT->interp para self-recursion (huge speedup
                         * en fib/factorial/etc.). */
                        bool is_self_ref = false;
                        if (fn_addr == 0
                         && opts_.mode == SelectorMode::VM_ABI
                         && !ins.func_name.empty()
                         && ins.func_name == ir_fn.name) {
                            is_self_ref = true;
                            is_user_call = true;
                            fn_addr = 0;  /* placeholder; se parchea post-encode */
                        }

                        /* D.4-fix: trampoline JIT->interp.  Si NO se resolvio
                         * como runtime ni como user-fn JIT-compilable, pero el
                         * symbol_table del .velb tiene un bytecode entry para
                         * "code.<sanitized_name>", emitir CALL al runtime
                         * wrapper @c vrt_call_bc_function que ejecuta el
                         * bytecode en mini-interp sincronico.  Esto permite
                         * que main + helpers basicos compilen aunque algunas
                         * callees (e.g. @c __new_<X> con raw_asm complejo)
                         * caigan a interp.  Cero overhead para callees ya
                         * JIT-compiladas (este path solo se toma cuando
                         * fn_addr==0 tras los dos intentos anteriores). */
                        bool is_bc_trampoline = false;
                        uint64_t bc_entry_va = 0;
                        if (fn_addr == 0
                         && !is_self_ref  /* Sprint fib-recursion: self-call no usa trampoline */
                         && opts_.runtime != nullptr
                         && opts_.runtime->call_bc_function != nullptr
                         && opts_.resolve_symbol
                         && opts_.mode == SelectorMode::VM_ABI) {
                            const std::string sym_key =
                                "code." + sanitize_label_name(ins.func_name);
                            bc_entry_va = opts_.resolve_symbol(sym_key);
                            if (bc_entry_va != 0) {
                                fn_addr = reinterpret_cast<uint64_t>(
                                    opts_.runtime->call_bc_function);
                                is_bc_trampoline = true;
                            }
                        }

                        if (fn_addr == 0 && !is_self_ref) {
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

                        /* Phase D.jit-mem-model VM-STACK: el abort por
                         * host_ptr-en-trampoline-arg YA NO es necesario:
                         *   - ALLOCAs en JIT producen VM-addrs (consistente
                         *     con interp), pasar al interp callee funciona.
                         *   - malloc/new producen host_ptrs genuinos
                         *     (is_host_ptr=true en IR), valid para plugins
                         *     que los esperan asi (e.g. STRRAW host_ptr).
                         * En ambos casos el interp callee ve un uint64
                         * apropiado para su semantica. */

                        /* Trampoline JIT->interp: marshalling VM_ABI igual que
                         * is_user_call (args en proc->regs[1..N], R15=nargs)
                         * pero la llamada nativa pasa (proc, bc_entry_va) en
                         * lugar de solo (proc). */
                        if (is_bc_trampoline) {
                            const size_t nargs = ins.operands.size();
                            const int32_t regs_base =
                                static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET);

                            /* Paso 1: stage args en proc->regs[1..N]. */
                            for (size_t a = 0; a < nargs && a < 12; ++a) {
                                load_op_rematerializable(mf, ir_fn, ins.operands[a], SCRATCH_A);
                                const int32_t off = regs_base +
                                    static_cast<int32_t>((a + 1) * 8);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, off),
                                        MOperand::make_reg(SCRATCH_A)));
                            }
                            /* Paso 2: R15 = nargs. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX, regs_base + 15 * 8),
                                    MOperand::make_imm32(static_cast<int32_t>(nargs))));
                            /* Paso 3: Native ABI args:
                             *   arg0 (rdi/rcx) = rbx (proc)
                             *   arg1 (rsi/rdx) = bc_entry_va (imm64 pool) */
#if defined(_WIN32)
                            const MReg n_arg0 = MReg::RCX;
                            const MReg n_arg1 = MReg::RDX;
#else
                            const MReg n_arg0 = MReg::RDI;
                            const MReg n_arg1 = MReg::RSI;
#endif
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(n_arg0),
                                    MOperand::make_reg(MReg::RBX)));
                            const uint32_t bc_pool_idx = mf.intern_imm64(bc_entry_va);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(n_arg1),
                                    MOperand::make_imm64_idx(bc_pool_idx)));
                            /* Paso 4: mov rax, fn_addr (vrt_call_bc_function);
                             * call rax + stackmap. */
                            const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(fn_pool_idx)));
                            MInstr tcall_instr;
                            tcall_instr.op = MOp::CALL;
                            tcall_instr.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(tcall_instr);
                            mf.blocks.back().instrs.push_back(tcall_instr);
                            /* Paso 5: el wrapper devuelve regs[0] en RAX, pero
                             * tambien lo ha escrito en proc->registers.regs[0]
                             * via el mini-interp.  El dst del IR puede leer
                             * directamente desde RAX -- mas barato que un
                             * extra load de memoria. */
                            if (ins.dst != ir::IR_NO_VALUE) {
                                store_op(mf, ins.dst, MReg::RAX);
                            }
                            if (jit::g_jit_warn_unsupported) {
                                auto key = std::make_pair(
                                    static_cast<int>(ins.op), ins.source_line);
                                if (warned_ops.insert(key).second) {
                                    std::fprintf(stderr,
                                        "[jit] selector: CALL '%s' via trampoline JIT->interp (callee no JIT-able) en fn '%s' linea %u\n",
                                        ins.func_name.c_str(),
                                        ir_fn.name.c_str(),
                                        ins.source_line);
                                }
                            }
                            emit_tailcall_epilogue();
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
                                load_op_rematerializable(mf, ir_fn, ins.operands[a], SCRATCH_A);
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
                            /* Sprint fib-recursion: marcar pool idx como
                             * self-ref si applicable.  Dedup contra dobles
                             * inserts (multi-site dentro de la misma fn
                             * comparte el mismo idx via intern_imm64). */
                            if (is_self_ref) {
                                bool already_marked = false;
                                for (uint32_t v : mf.self_ref_imm64_indices) {
                                    if (v == fn_pool_idx) { already_marked = true; break; }
                                }
                                if (!already_marked) {
                                    mf.self_ref_imm64_indices.push_back(fn_pool_idx);
                                }
                            }
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
                            emit_tailcall_epilogue();
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
                            load_op_rematerializable(mf, ir_fn, ins.operands[a], ARG_REGS[a]);
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
                        emit_tailcall_epilogue();
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
                        /* Sprint alloc-inline (2026-06-02): inline nursery
                         * bump-pointer alloc cuando los offsets estan
                         * resueltos.  El fast-path emite:
                         *
                         *   mov  rdx, [class_slot]            ; cls
                         *   mov  ecx, [rdx + 24]              ; instance_size (u32)
                         *   add  ecx, 8                       ; + GcHeader
                         *   add  ecx, 7                       ; round up to 8
                         *   and  ecx, ~7
                         *   mov  rax, [rbx + nursery_bump]    ; bump
                         *   lea  r8,  [rax + rcx]             ; new_bump
                         *   cmp  r8,  [rbx + nursery_end]
                         *   ja   slow_path
                         *   mov  [rbx + nursery_bump], r8     ; commit bump
                         *   mov  [rax], ecx                   ; GcHeader.size
                         *   mov  dword [rax + 4], 0           ; color/gen = 0
                         *   mov  [rax + 8], rdx               ; ObjectHeader.class_ptr
                         *   mov  dword [rax + 16], 1          ; flags = GC_OWNED
                         *   ; (proc=rbx, raw=rax ya OK para Native ABI)
                         *   mov  rdi/rcx, rbx                 ; arg0
                         *   mov  rsi/rdx, rax                 ; arg1
                         *   call vrt_register_alloc           ; -> handle en RAX
                         *   jmp  done
                         * slow_path:
                         *   mov  arg0, rbx; mov arg1, cls
                         *   call vrt_newobj_handle            ; -> handle en RAX
                         * done:
                         *   mov [slot dst], rax
                         *
                         * Ahorra: bump + init headers inline (~3 ns) vs
                         * la version vieja que entraba TODA al runtime
                         * (~25 ns).  Net ~20 ns por alloc.
                         */
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
                        /* Sprint alloc-inline REVERT (2026-06-02): el
                         * inline bump-pointer + call register_alloc no
                         * trajo ganancia neta porque register_alloc
                         * (new_handle) cuesta lo mismo que el call
                         * directo a vrt_newobj_handle.  Y el inline
                         * anyade ~14 instrs por iter que CAUSAN
                         * regresion ~20-30% en hot loops de NEWOBJ.
                         *
                         * Forzar el path "directo" (call a
                         * vrt_newobj_handle) siempre, hasta que tengamos
                         * un esquema de handle mas barato que justifique
                         * el inline. */
                        const bool can_inline = false;
                        (void)opts_.nursery_bump_offset;
                        (void)opts_.nursery_end_offset;
                        (void)opts_.register_alloc_addr;

                        if (!can_inline) {
                            /* Fallback: call vrt_newobj_handle (path viejo). */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], ABI1);
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
                            if (ins.dst != ir::IR_NO_VALUE) {
                                store_op(mf, ins.dst, MReg::RAX);
                            }
                            break;
                        }

                        /* INLINE FAST-PATH.
                         * Layout regs:
                         *   ABI1 (RDX win / RSI linux) = class_ptr
                         *   RAX  = bump (objeto recien alocado, raw ptr)
                         *   RCX  = total size (u32, escalado a 8 bytes)
                         *   R8   = new_bump
                         *   R9   = scratch
                         */
                        const MLabelId lbl_slow = mf.new_label();
                        const MLabelId lbl_done = mf.new_label();

                        /* Cargar class_ptr a ABI1. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], ABI1);

                        /* mov ecx, [ABI1 + 24]  ; instance_size u32 (zero-ext a rcx). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RCX, 4),
                                MOperand::make_mem(ABI1, 24)));
                        /* add rcx, 15 (= 8 GcHeader + 7 padding) */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::ADD,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_imm32(15)));
                        /* and rcx, ~7 (align 8) */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::AND,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_imm32(static_cast<int32_t>(~7))));
                        /* mov rax, [rbx + nursery_bump_off] */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_mem(MReg::RBX,
                                    opts_.nursery_bump_offset)));
                        /* mov r8, rax ; new_bump = bump */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R8),
                                MOperand::make_reg(MReg::RAX)));
                        /* add r8, rcx */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_binary(MOp::ADD,
                                MOperand::make_reg(MReg::R8),
                                MOperand::make_reg(MReg::R8),
                                MOperand::make_reg(MReg::RCX)));
                        /* cmp r8, [rbx + nursery_end_off] */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_binary(MOp::CMP,
                                MOperand{},
                                MOperand::make_reg(MReg::R8),
                                MOperand::make_mem(MReg::RBX,
                                    opts_.nursery_end_offset)));
                        /* ja lbl_slow */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::A, lbl_slow));
                        /* mov [rbx + nursery_bump_off], r8 ; commit */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX,
                                    opts_.nursery_bump_offset),
                                MOperand::make_reg(MReg::R8)));
                        /* GcHeader init: mov dword [rax], ecx (size). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RAX, 0),
                                MOperand::make_reg(MReg::RCX, 4)));
                        /* mov dword [rax+4], 0  ; color=WHITE/gen=YOUNG/etc.
                         * Usar reg de 32 bits para que el encoding sea dword. */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RAX, 4),
                                MOperand::make_imm32(0)));
                        /* ObjectHeader.class_ptr = ABI1 (offset 8 desde raw). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RAX, 8),
                                MOperand::make_reg(ABI1)));
                        /* ObjectHeader.flags (offset 16) = OBJ_FLAG_GC_OWNED (=1). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RAX, 16),
                                MOperand::make_imm32(1)));
                        /* Llamar vrt_register_alloc(proc=rbx, raw=rax).
                         * Native ABI: arg0=ABI0, arg1=ABI1.
                         * Pero RAX es el ptr (no podemos copiar a ABI1
                         * todavia porque ABI1 ya contiene cls; tras este
                         * call, cls ya no se necesita). */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ABI1),
                                MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ABI0),
                                MOperand::make_reg(MReg::RBX)));
                        {
                            const uint32_t pidx = mf.intern_imm64(
                                opts_.register_alloc_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(pidx)));
                        }
                        {
                            MInstr c1;
                            c1.op = MOp::CALL;
                            c1.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(c1);
                            mf.blocks.back().instrs.push_back(c1);
                        }
                        /* jmp done */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jmp(lbl_done));

                        /* slow_path: */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_label_def(lbl_slow));
                        /* Recargar class_ptr (puede haber sido clobbered). */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], ABI1);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ABI0),
                                MOperand::make_reg(MReg::RBX)));
                        {
                            const uint32_t pidx = mf.intern_imm64(
                                reinterpret_cast<uint64_t>(
                                    opts_.runtime->newobj_handle));
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(pidx)));
                        }
                        {
                            MInstr c1;
                            c1.op = MOp::CALL;
                            c1.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(c1);
                            mf.blocks.back().instrs.push_back(c1);
                        }
                        /* done: */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_label_def(lbl_done));
                        if (ins.dst != ir::IR_NO_VALUE) {
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    case IrOp::CALLN: {
                        /* Math-IR-promote v2.2c interception: el pre-pase del
                         * IR emitter convierte IABS/IMIN/IMAX/IMINU/IMAXU/
                         * ILOG2 (y otros) a CALLN a vmath_* porque el bytecode
                         * VM no tiene opcode nativo.  Para el JIT, sin embargo,
                         * podemos emitir cmov/sar/xor inline directo (~5 instr
                         * vs ~50ns CALLN).  Detectamos por func_name. */
                        if (ins.func_name == "stdlib/native/math/vesta_math:vmath_abs"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_min"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_max"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_minu"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_maxu"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_ilog2"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_rotl"
                         || ins.func_name == "stdlib/native/math/vesta_math:vmath_rotr") {
                            const std::string &fn = ins.func_name;
                            if (fn.find("vmath_abs") != std::string::npos) {
                                if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                                    unsupported = true;
                                    mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                    break;
                                }
                                load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_reg(SCRATCH_A)));
                                /* sar rdx, 63 -- forma 2-operand (dst, imm). */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::SAR,
                                        MOperand::make_reg(SCRATCH_C),
                                        MOperand::make_imm32(63)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::XOR,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_C)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::SUB,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_C)));
                                store_op(mf, ins.dst, SCRATCH_A);
                                break;
                            }
                            if (fn.find("vmath_ilog2") != std::string::npos) {
                                if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                                    unsupported = true;
                                    mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                    break;
                                }
                                load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::LZCNT,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_reg(SCRATCH_B)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::NEG,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_reg(MReg::RAX)));
                                /* add rax, 63 -- usar SUB con neg trick o
                                 * ADD r/imm32 (forma 2-op: dst, imm32). */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(MReg::RAX),
                                        MOperand::make_imm32(63)));
                                store_op(mf, ins.dst, MReg::RAX);
                                break;
                            }
                            /* ROTL/ROTR: cuenta en CL (RCX low byte).  El
                             * encoder de SHL/SAR ya emite `rol/ror r64, cl`
                             * cuando src es REG==RCX.  Patron:
                             *   load val -> RAX
                             *   load cnt -> RCX (CL ya esta listo)
                             *   rol/ror rax, cl
                             *   store dst <- rax  */
                            if (fn.find("vmath_rotl") != std::string::npos
                             || fn.find("vmath_rotr") != std::string::npos) {
                                if (ins.dst == ir::IR_NO_VALUE
                                 || ins.operands.size() < 2) {
                                    unsupported = true;
                                    mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                    break;
                                }
                                load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                                load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                                const MOp rot_op = (fn.find("vmath_rotl") != std::string::npos)
                                    ? MOp::ROL : MOp::ROR;
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(rot_op,
                                        MOperand::make_reg(SCRATCH_A),
                                        MOperand::make_reg(SCRATCH_B)));
                                store_op(mf, ins.dst, SCRATCH_A);
                                break;
                            }
                            /* IMIN/IMAX/IMINU/IMAXU. */
                            if (ins.dst == ir::IR_NO_VALUE || ins.operands.size() < 2) {
                                unsupported = true;
                                mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                break;
                            }
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                            load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::CMP,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_reg(SCRATCH_B)));
                            MCond cc;
                            if (fn.find("vmath_minu") != std::string::npos)      cc = MCond::A;
                            else if (fn.find("vmath_maxu") != std::string::npos) cc = MCond::B;
                            else if (fn.find("vmath_min")  != std::string::npos) cc = MCond::G;
                            else                                                  cc = MCond::L; /* vmath_max */
                            {
                                MInstr i;
                                i.op = MOp::CMOVCC;
                                i.variant = static_cast<uint8_t>(cc);
                                i.dst  = MOperand::make_reg(SCRATCH_A);
                                i.src1 = MOperand::make_reg(SCRATCH_B);
                                mf.blocks.back().instrs.push_back(i);
                            }
                            store_op(mf, ins.dst, SCRATCH_A);
                            break;
                        }

                        /* Sprint JIT-cross-fn 2026-06-01: CALLNI (FFI runtime
                         * indirect).  El frontend emite IrOp::CALLN con
                         * func_name="__callni__:" y operands[0] = host_ptr
                         * al simbolo nativo (resuelto en runtime via
                         * ffi_sym/dlsym).  operands[1..] son los args.  El
                         * selector lo trata como un CALL normal pero con
                         * fn_addr cargado desde el SSA value en lugar del
                         * resolver compile-time. */
                        const bool is_callni =
                            ins.func_name.size() >= 11
                         && ins.func_name.compare(0, 11, "__callni__:") == 0;

                        if (is_callni) {
                            if (ins.operands.empty()) {
                                warn_unsupported(ins.op, ins.source_line,
                                    "CALLNI sin fn_ptr operand");
                                unsupported = true;
                                mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                break;
                            }
                            /* Native ABI arg regs. */
#if defined(_WIN32)
                            static const MReg N_ARG_REGS_NI[] = {
                                MReg::RCX, MReg::RDX, MReg::R8, MReg::R9
                            };
                            constexpr size_t N_MAX_REG_ARGS_NI = 4;
#else
                            static const MReg N_ARG_REGS_NI[] = {
                                MReg::RDI, MReg::RSI, MReg::RDX,
                                MReg::RCX, MReg::R8, MReg::R9
                            };
                            constexpr size_t N_MAX_REG_ARGS_NI = 6;
#endif
                            const size_t nargs_ni = ins.operands.size() - 1;
                            if (nargs_ni > N_MAX_REG_ARGS_NI) {
                                warn_unsupported(ins.op, ins.source_line,
                                    "CALLNI con demasiados args (stack args no impl)");
                                unsupported = true;
                                mf.blocks.back().instrs.push_back({MOp::INT3, 0, 0, 0, {}, {}, {}});
                                break;
                            }
                            /* Guardar fn_ptr en R11 (caller-saved, no es arg
                             * reg en ningun ABI, no clobbered por load_op
                             * que usa RAX/RCX/RDX como scratch).  Asi evitamos
                             * push/pop que podrian desalinear el stack. */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::R11);
                            /* Cargar args a N_ARG_REGS.  load_op puede usar
                             * RAX como scratch pero NO R11. */
                            for (size_t a = 0; a < nargs_ni; ++a) {
                                load_op_rematerializable(mf, ir_fn, ins.operands[a + 1], N_ARG_REGS_NI[a]);
                            }
                            /* mov rax, r11  (CALL via rax) */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_reg(MReg::R11)));
                            /* CALL rax + stackmap. */
                            MInstr call_ni_instr;
                            call_ni_instr.op = MOp::CALL;
                            call_ni_instr.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(call_ni_instr);
                            mf.blocks.back().instrs.push_back(call_ni_instr);
                            if (ins.dst != ir::IR_NO_VALUE) {
                                store_op(mf, ins.dst, MReg::RAX);
                            }
                            break;
                        }

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
                            load_op_rematerializable(mf, ir_fn, ins.operands[a], N_ARG_REGS[a]);
                        }

                        /* mov rax, fn_addr (via imm64 pool). */
                        const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(fn_pool_idx)));

                        /* Win64 shadow space (home space) en call sites de
                         * funciones con host-ALLOCAs.
                         *
                         * El callee nativo puede spillar sus args registrados a
                         * los 32 bytes de "home space" en [rsp, rsp+32).  El
                         * prologo reserva esos 32 bytes al fondo del frame, PERO
                         * un ALLOCA host (`sub rsp, N`) mueve rsp por debajo de
                         * ese fondo, dejando el home space del callee SOLAPADO
                         * con los datos del ALLOCA -> el spill los corrompe.
                         * (Sintoma: el array de qsort sobrescrito con punteros/
                         *  PCs cuando el callback compile -- C++ pesado -- spillaba
                         *  fn_pc al home space que pisaba el array.)
                         *
                         * Fix: reservar 32 bytes frescos justo antes del call y
                         * liberarlos despues, SOLO cuando la fn tiene host-ALLOCAs
                         * (sino el home del prologo es valido).  0x20 = multiplo
                         * de 16 -> preserva el alignment del rsp.  SysV no tiene
                         * home space, asi que es no-op fuera de Win64. */
#if defined(_WIN32)
                        const bool cn_need_shadow = fn_has_alloca;
                        if (cn_need_shadow) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::SUB,
                                    MOperand::make_reg(MReg::RSP),
                                    MOperand::make_imm32(0x20)));
                        }
#else
                        const bool cn_need_shadow = false;
#endif

                        /* CALL rax + stackmap (el plugin nativo podria
                         * triggear GC indirectamente). */
                        MInstr call_instr;
                        call_instr.op = MOp::CALL;
                        call_instr.src1 = MOperand::make_reg(MReg::RAX);
                        emit_stackmap_for_safepoint(call_instr);
                        mf.blocks.back().instrs.push_back(call_instr);

                        if (cn_need_shadow) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::ADD,
                                    MOperand::make_reg(MReg::RSP),
                                    MOperand::make_imm32(0x20)));
                        }

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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 8),
                                MOperand::make_reg(SCRATCH_A)));
                        for (size_t a = 0; a < nargs; ++a) {
                            load_op_rematerializable(mf, ir_fn, ins.operands[a + 1], SCRATCH_A);
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

                        /* POLYMORPHIC INLINE CACHE (PIC, hasta 4 entries).
                         *
                         * Layout del slot (64 bytes, cache-line aligned):
                         *   +0  [class_0][jit_code_0]
                         *   +16 [class_1][jit_code_1]
                         *   +32 [class_2][jit_code_2]
                         *   +48 [class_3][jit_code_3]
                         *
                         * Patron emitido:
                         *   load obj -> rax
                         *   mov rcx, [rax+0]            ; class_ptr
                         *   mov r10, slot_base          ; imm64
                         *   cmp rcx, [r10 + 0];  je hit_call
                         *     mov rax, [r10 + 8]
                         *   cmp rcx, [r10 + 16]; je hit_call_1
                         *     mov rax, [r10 + 24]
                         *   cmp rcx, [r10 + 32]; je hit_call_2
                         *     mov rax, [r10 + 40]
                         *   cmp rcx, [r10 + 48]; je hit_call_3
                         *     mov rax, [r10 + 56]
                         *   jmp ic_miss
                         * hit_call_N: -- todos convergen aqui con rax=jit_code
                         *   mov rcx, rbx
                         *   call rax
                         *   jmp continue
                         * ic_miss:
                         *   call vrt_callvirt_ic(proc, obj, vtbl_idx, slot_base)
                         *   jmp continue
                         *
                         * Coste por hit: 1 load class + N cmp/je (1-4 segun
                         * orden de las entries) + 1 load jit_code + call.
                         * Una clase NUEVA en orden 0 = 5 instr (similar al MIC).
                         * 4 clases distintas alternando = ~10-13 instr promedio.
                         * Coste por miss: vrt_callvirt_ic + actualizar slot.
                         */
                        uint64_t ic_slot_addr = 0;
                        if (opts_.reserve_ic_slot) {
                            ic_slot_addr = opts_.reserve_ic_slot(next_ic_key());
                        }
                        if (ic_slot_addr != 0 && opts_.runtime
                         && opts_.runtime->callvirt_ic) {
                            const MLabelId ic_miss_label     = mf.new_label();
                            const MLabelId ic_continue_label = mf.new_label();
                            const MLabelId hit_call_label    = mf.new_label();
                            /* load obj */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                            /* mov rcx, [rax+0]  (class_ptr) */
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_mem(MReg::RAX, 0)));
                            /* mov r10, ic_slot_addr (imm64 via pool) */
                            const uint32_t ic_idx = mf.intern_imm64(ic_slot_addr);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R10),
                                MOperand::make_imm64_idx(ic_idx)));
                            /* 4 entries, secuencia para cada una:
                             *   cmp rcx, [r10 + 16*e]
                             *   jne skip_e
                             *   mov rax, [r10 + 16*e + 8]
                             *   jmp hit_call
                             *   skip_e:
                             * Tras la 4ta entry, si nada matcheo, jmp ic_miss. */
                            for (int e = 0; e < 4; ++e) {
                                const int32_t class_off = e * 16;
                                const int32_t code_off  = e * 16 + 8;
                                const MLabelId skip_e_label = mf.new_label();
                                /* cmp rcx, [r10 + class_off] */
                                mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::CMP,
                                    MOperand::make_reg(MReg::RCX),
                                    MOperand::make_mem(MReg::R10, class_off)));
                                /* jne skip_e */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(MCond::NE, skip_e_label));
                                /* mov rax, [r10 + code_off] */
                                mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_mem(MReg::R10, code_off)));
                                /* jmp hit_call */
                                mf.blocks.back().instrs.push_back(MInstr::make_jmp(hit_call_label));
                                /* skip_e: */
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_label_def(skip_e_label));
                            }
                            /* Si llegamos aqui, ninguna entry matcheo -> miss. */
                            mf.blocks.back().instrs.push_back(MInstr::make_jmp(ic_miss_label));
                            /* hit_call: convergencia de los 4 hits con rax = jit_code. */
                            mf.blocks.back().instrs.push_back(MInstr::make_label_def(hit_call_label));
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_reg(MReg::RBX)));
                            {
                                MInstr hit_call;
                                hit_call.op = MOp::CALL;
                                hit_call.src1 = MOperand::make_reg(MReg::RAX);
                                emit_stackmap_for_safepoint(hit_call);
                                mf.blocks.back().instrs.push_back(hit_call);
                            }
                            mf.blocks.back().instrs.push_back(MInstr::make_jmp(ic_continue_label));
                            /* ic_miss: */
                            mf.blocks.back().instrs.push_back(MInstr::make_label_def(ic_miss_label));
                            /* Setup args para vrt_callvirt_ic(proc, obj, idx, slot). */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);  /* obj */
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);  /* obj host_ptr */
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
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::RBX, regs_base + 8),
                                MOperand::make_reg(SCRATCH_A)));
                        for (size_t a = 0; a < nargs; ++a) {
                            load_op_rematerializable(mf, ir_fn, ins.operands[a + 2], SCRATCH_A);
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

                        /* INLINE CALLM DISPATCH (Phase D.8): el metodo ya esta
                         * RESUELTO en operands[1] (por un FINDMETHOD previo) ->
                         * no hay vtable walk como en CALLVIRT.  En vez de llamar
                         * a vrt_callm (que re-setea regs[1] + on-demand-compile +
                         * enter_jit por iteracion -- ~150ns en pic_real, 115x vs
                         * C++), inline: cargar method->jit_code; si != null y sin
                         * advice -> CALL DIRECTO (los args ya estan en
                         * proc->registers, staged arriba); si no -> fallback a
                         * vrt_callm.  Mismo patron que el inline de CALLVIRT. */
                        const MLabelId cm_fallback = mf.new_label();
                        const MLabelId cm_continue = mf.new_label();
                        /* mov rcx, method (operands[1]) */
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], MReg::RCX);
                        /* test rcx; jz fallback (method null) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RCX),
                            MOperand::make_reg(MReg::RCX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, cm_fallback));
                        /* mov rax, [rcx + ADVICE_CHAIN_OFFSET]; test rax; jnz fallback
                         * (AOP: si el metodo tiene advices, el fast path inline no
                         * los aplica -> caer al slow path vrt_callm). */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RCX, VESTA_METHODINFO_ADVICE_CHAIN_OFFSET)));
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::NE, cm_fallback));
                        /* mov rax, [rcx + JIT_CODE_OFFSET]; test rax; jz fallback */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RCX, VESTA_METHODINFO_JIT_CODE_OFFSET)));
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::E, cm_fallback));
                        /* FAST PATH: mov rcx, rbx (proc); call rax (jit_code).
                         * Los args ya estan en proc->registers (staged arriba);
                         * el metodo VM_ABI los lee. */
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
                        mf.blocks.back().instrs.push_back(MInstr::make_jmp(cm_continue));

                        /* fallback: vrt_callm(proc, obj, method). */
                        mf.blocks.back().instrs.push_back(MInstr::make_label_def(cm_fallback));
                        /* Native ABI: arg0=proc(rbx), arg1=obj_payload, arg2=method_ptr.
                         * R10/R11 (caller-saved, no arg regs) como temporales para
                         * evitar clobber (RDX es cm_arg1 Win64 / cm_arg2 SysV). */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::R10);  /* obj host_ptr */
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], MReg::R11);  /* method ptr */
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

                        /* continue: ambos paths convergen con resultado en RAX. */
                        mf.blocks.back().instrs.push_back(MInstr::make_label_def(cm_continue));
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
                            load_op_rematerializable(mf, ir_fn, ins.operands[a + 1], SCRATCH_A);
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
                        load_op_rematerializable(mf, ir_fn, ins.func_ptr,    MReg::R10);   /* fn_addr */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::R11);   /* env_addr */
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
                                        load_op_rematerializable(mf, ir_fn, arg.value, SCRATCH_A);
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

                        /* Sprint CCC: fusion CMP+BR_COND.  Si el CMP_*
                         * anterior nos paso operands+cond, emitimos cmp
                         * DESPUES del safepoint poll para no clobrear flags. */
                        const bool use_fused =
                            fused_cmp_active
                         && fused_cmp_dst == ins.operands[0];

                        /* SAFEPOINT antes del condicional si alguna rama es back-edge.
                         * Importante: emitimos PRIMERO el safepoint (que toca
                         * flags via cmp byte [rbx],0) y LUEGO el cmp fusionado
                         * o el load+test estandar. */
                        const bool is_back =
                            opts_.mode == SelectorMode::VM_ABI &&
                            ((ins.target_block <= static_cast<ir::IrBlockId>(bi)) ||
                             (ins.false_block  <= static_cast<ir::IrBlockId>(bi)));
                        if (is_back && safepoint_pool_idx_ != UINT32_MAX) {
                            MInstr sp_instr = MInstr::make_safepoint(safepoint_pool_idx_);
                            emit_stackmap_for_safepoint(sp_instr);
                            mf.blocks.back().instrs.push_back(sp_instr);
                        }

                        if (use_fused) {
                            /* Emitir cmp DESPUES del safepoint para que las
                             * flags lleguen vivas al jcc. */
                            load_op_rematerializable(mf, ir_fn, fused_cmp_op0, SCRATCH_B);
                            load_op_rematerializable(mf, ir_fn, fused_cmp_op1, SCRATCH_C);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::CMP,
                                    MOperand::make_reg(SCRATCH_B),
                                    MOperand::make_reg(SCRATCH_C)));
                        } else {
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
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
                                        load_op_rematerializable(mf, ir_fn, arg.value, SCRATCH_A);
                                        store_op(mf, pin.dst, SCRATCH_A);
                                        break;
                                    }
                                }
                            }
                        };

                        /* Si NO esta fusionado, emitir test rax,rax (load+test).
                         * Si SI esta fusionado, las flags ya estan del CMP
                         * emitido arriba y mov-based PHI copies no las tocan. */
                        const auto invert_cond = [](MCond c) -> MCond {
                            switch (c) {
                                case MCond::E:  return MCond::NE;
                                case MCond::NE: return MCond::E;
                                case MCond::L:  return MCond::GE;
                                case MCond::GE: return MCond::L;
                                case MCond::G:  return MCond::LE;
                                case MCond::LE: return MCond::G;
                                case MCond::B:  return MCond::AE;
                                case MCond::AE: return MCond::B;
                                case MCond::A:  return MCond::BE;
                                case MCond::BE: return MCond::A;
                                default:        return MCond::NE;
                            }
                        };
                        const MCond taken_cond = use_fused ? fused_cmp_cond  : MCond::NE;
                        const MCond skip_cond  = use_fused ? invert_cond(fused_cmp_cond) : MCond::E;
                        if (!use_fused) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::TEST,
                                    MOperand::make_reg(SCRATCH_A),
                                    MOperand::make_reg(SCRATCH_A)));
                        }

                        if (!t_has_phi && !f_has_phi) {
                            /* Fast path: no PHIs, codigo identico al original. */
                            if (ins.target_block < block_labels.size()) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(taken_cond,
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
                                MInstr::make_jcc(skip_cond, skip_taken_lbl));
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
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                        }
                        /* VM_ABI: ademas escribir RAX a proc->registers.regs[0]
                         * para que el caller bytecode/interprete vea el return.
                         * NATIVE_ABI: nada extra, return en RAX como convencion C.
                         * callback-ABI: el return va en RAX (convencion C) y NO
                         * se escribe proc->regs[R0]; en su lugar se restaura el
                         * banco VM salvado (re-entrancia).  Todo con SCRATCH_B
                         * (rcx) para preservar RAX. */
                        if (cb_entry) {
                            /* SAFE: restaurar proc->registers[0..15] del save area. */
                            if (cb_save_all) {
                                for (uint32_t r = 0; r < 16; ++r) {
                                    const int32_t reg_off = VESTA_PROC_REGISTERS_OFFSET
                                        + static_cast<int32_t>(r) * VESTA_REGISTER_SIZE;
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_B),
                                            MOperand::make_mem(MReg::RBP, cb_save_off(r))));
                                    mf.blocks.back().instrs.push_back(
                                        MInstr::make_unary(MOp::MOV,
                                            MOperand::make_mem(MReg::RBX, reg_off),
                                            MOperand::make_reg(SCRATCH_B)));
                                }
                            }
                            /* Restaurar VM-RSP si la fn uso ALLOCAs. */
                            if (fn_has_alloca) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV, MOperand::make_reg(SCRATCH_B),
                                        MOperand::make_mem(MReg::RBP, vm_rsp_save_off)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                                        MOperand::make_reg(SCRATCH_B)));
                            }
                        } else if (opts_.mode == SelectorMode::VM_ABI) {
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::RBX,
                                        VESTA_PROC_REGISTERS_OFFSET),
                                    MOperand::make_reg(MReg::RAX)));
                            /* Phase D.jit-mem-model VM-STACK: restaurar
                             * VM-RSP original si la fn modifico vm_stack
                             * via ALLOCAs.  Usa SCRATCH_B (rcx) para no
                             * clobbear RAX (return value). */
                            if (fn_has_alloca) {
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(SCRATCH_B),
                                        MOperand::make_mem(MReg::RBP, vm_rsp_save_off)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                                        MOperand::make_reg(SCRATCH_B)));
                            }
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

                    case ir::IrOp::READ_VM_REG: {
                        /* raw_asm-elim wave 3: lee proc->registers.regs[imm].qword().
                         * Layout: VESTA_PROC_REGISTERS_OFFSET + N*VESTA_REGISTER_SIZE
                         * (declarado en vesta_rt/abi.h: REGISTERS_OFFSET=96, SIZE=8).
                         * El acceso es desde RBX (proc) + offset, sin runtime call. */
                        if (ins.dst != ir::IR_NO_VALUE && ins.imm <= 15) {
                            const int32_t reg_offset = static_cast<int32_t>(
                                VESTA_PROC_REGISTERS_OFFSET
                                + static_cast<int64_t>(ins.imm) * VESTA_REGISTER_SIZE);
                            /* mov rax, [rbx + reg_offset]. */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_mem(MReg::RBX, reg_offset)));
                            store_op(mf, ins.dst, MReg::RAX);
                        }
                        break;
                    }

                    /* FINDMETHOD / FINDFIELD: lookup por nombre en ClassRegistry.
                     * Usado por interface dispatch (`shape.area()` con shape
                     * tipado como interfaz) y reflexion runtime.
                     *
                     * Patron:
                     *   load params_vaddr -> arg1
                     *   mov arg0, rbx (proc)
                     *   call vrt_findmethod / vrt_findfield
                     *   store dst <- rax (MethodInfo* / FieldInfo*)
                     *
                     * Marca el dst como host_ptr (MethodInfo es host memoria). */
                    case ir::IrOp::FINDMETHOD:
                    case ir::IrOp::FINDFIELD: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FINDMETHOD/FINDFIELD: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t runtime_fn = (ins.op == ir::IrOp::FINDMETHOD)
                            ? (opts_.runtime ? reinterpret_cast<uint64_t>(opts_.runtime->findmethod) : 0)
                            : (opts_.runtime ? reinterpret_cast<uint64_t>(opts_.runtime->findfield)  : 0);
                        if (runtime_fn == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->findmethod/findfield no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* Native ABI: arg0=proc, arg1=params_vaddr. */
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX;
                        const MReg arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI;
                        const MReg arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0),
                            MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx  = mf.intern_imm64(runtime_fn);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    case ir::IrOp::RETHROW:
                    case ir::IrOp::SHARED_STAT:
                    case ir::IrOp::RSPAWN_RETURN:
                    case ir::IrOp::SMARTPTR_FREE:
                    case ir::IrOp::REFLECT_COUNT:
                    case ir::IrOp::REFLECT_AT:
                    case ir::IrOp::MOD_LOAD:
                    case ir::IrOp::DLOPEN:
                    case ir::IrOp::DLSYM: {
                        /* raw_asm-elim wave 2+3: 4 ops nuevos no soportados
                         * en JIT v1.  Caen a unsupported -> el bytecode interp
                         * los maneja correctamente.  Phase D.13 (native
                         * unwinding) cubrira RETHROW; SHARED_STAT, SMARTPTR_FREE
                         * y RSPAWN_RETURN son de baja frecuencia y aparecen
                         * en cleanups (no en hot path). */
                        warn_unsupported(ins.op, ins.source_line,
                            "wave-3 IR op no implementada en Selector v1");
                        unsupported = true;
                        mf.blocks.back().instrs.push_back(
                            {MOp::INT3, 0, 0, 0, {}, {}, {}});
                        break;
                    }

                    /* ==================================================
                     * Math-IR-promote v2.2a: bit ops nativos en JIT.
                     *
                     * Cada uno baja a 1 instr nativa x86 (1-4 ciclos):
                     *   POPCNT: F3 0F B8  popcnt rd, rs    (3c, SSE4.2)
                     *   CLZ:    F3 0F BD  lzcnt rd, rs     (3c, BMI1)
                     *   CTZ:    F3 0F BC  tzcnt rd, rs     (3c, BMI1)
                     *   BSWAP:  0F C8+rd  bswap rd         (2c, baseline)
                     *   ROTL:   D1/C1 /0  rol rd, imm      (1c)
                     *   ROTR:   D1/C1 /1  ror rd, imm      (1c)
                     *
                     * Compara vs CALLN a vmath_*: ~50ns -> ~3ns = 15-20x.
                     * ================================================== */
                    case ir::IrOp::POPCNT:
                    case ir::IrOp::CLZ:
                    case ir::IrOp::CTZ: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* Bug fix 2026-05-31: usar SCRATCH_B (RCX) como src
                         * y RAX como dst para evitar que el regalloc rewrite
                         * elimine un MOV RAX, RAX falsamente identidad.
                         *
                         * Sintoma original: cuando el regalloc tenia el
                         * operando pinned a un reg distinto Y la instr previa
                         * dejaba un valor STALE en RAX, el load_op(op0, RAX)
                         * se rewriteaba a `MOV RAX, R<pinned>` pero por algun
                         * motivo no aparecia en la salida, dejando RAX con el
                         * valor anterior (e.g. resultado de un ADD previo).
                         * Patron seguro: cargar a RCX (distinto del dst RAX),
                         * luego `popcnt RAX, RCX`. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        const MOp mop =
                            (ins.op == ir::IrOp::POPCNT) ? MOp::POPCNT :
                            (ins.op == ir::IrOp::CLZ)    ? MOp::LZCNT :
                                                            MOp::TZCNT;
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(mop,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(SCRATCH_B)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::BYTESWAP: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* BSWAP es unario in-place (solo dst).  Cargar a
                         * RCX primero, copiar a RAX, bswap RAX, store.  El
                         * copy intermedio garantiza que RAX recibe el valor
                         * recien cargado (no uno stale). */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::BSWAP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::RAX)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    /* ==================================================
                     * Math-IR-promote v2.2c+: IABS/IMIN/IMAX nativos en JIT
                     * via cmov.  Antes caian al pre-pase (CALLN a vmath_*).
                     *
                     * Patrones:
                     *   IABS:    mov rax, src
                     *            mov rdx, rax
                     *            sar rdx, 63        ; rdx = -1 si neg, 0 si pos
                     *            xor rax, rdx       ; flip bits si neg
                     *            sub rax, rdx       ; +1 si neg => |x|
                     *
                     *   IMIN/IMAX/IMINU/IMAXU:
                     *            mov rax, a
                     *            mov rcx, b
                     *            cmp rax, rcx
                     *            cmov<cond> rax, rcx
                     *     IMIN signed:    cmovg  (rax > rcx -> tomar rcx)
                     *     IMAX signed:    cmovl  (rax < rcx -> tomar rcx)
                     *     IMINU unsigned: cmova  (rax > rcx u -> tomar rcx)
                     *     IMAXU unsigned: cmovb  (rax < rcx u -> tomar rcx)
                     *
                     * Coste: ~4-5 instr maquina vs ~50ns por CALLN.  ~10-15x.
                     * ================================================== */
                    case ir::IrOp::IABS: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "IABS: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* Branchless abs (3 ops + 2 movs).
                         * Mas rapido que neg+cmov porque evita la dep
                         * sobre flags.  Funciona para todos los i64 excepto
                         * INT_MIN (mismo undef que el IR docea). */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);  /* rax = x */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(SCRATCH_C),
                                MOperand::make_reg(SCRATCH_A)));  /* rdx = x */
                        /* sar rdx, 63 -> rdx = -1 si neg, 0 si pos */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::SAR,
                                MOperand::make_reg(SCRATCH_C),
                                MOperand::make_imm32(63)));
                        /* xor rax, rdx */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::XOR,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_C)));
                        /* sub rax, rdx */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::SUB,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_C)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::IMIN:
                    case ir::IrOp::IMAX:
                    case ir::IrOp::IMINU:
                    case ir::IrOp::IMAXU: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "IMIN/IMAX: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);  /* rax = a */
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);  /* rcx = b */
                        /* cmp rax, rcx */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CMP,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_B)));
                        /* cmov<cc> rax, rcx */
                        const MCond cc =
                            (ins.op == ir::IrOp::IMIN)  ? MCond::G  :
                            (ins.op == ir::IrOp::IMAX)  ? MCond::L  :
                            (ins.op == ir::IrOp::IMINU) ? MCond::A  :
                                                           MCond::B;
                        {
                            MInstr i;
                            i.op = MOp::CMOVCC;
                            i.variant = static_cast<uint8_t>(cc);
                            i.dst  = MOperand::make_reg(SCRATCH_A);
                            i.src1 = MOperand::make_reg(SCRATCH_B);
                            mf.blocks.back().instrs.push_back(i);
                        }
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::ILOG2: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "ILOG2: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* ilog2(u64 x) = 63 - clz(x).  Asumimos x != 0 (UB
                         * documentada).  Emitimos lzcnt + neg + add 63. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::LZCNT,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(SCRATCH_B)));
                        /* rax = 63 - rax  ==> neg rax; add rax, 63 */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::NEG,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::ADD,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm32(63)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    /* ==================================================
                     * Math-IR-promote v2.2b: FP nativas en JIT con XMM.
                     *
                     * Patron memory-roundtrip via MOVQ:
                     *   load_op(op0, RCX)        ; rcx <- bits f64 desde slot
                     *   MOVQ_GP_XMM XMM0, RCX    ; xmm0 = rcx (4 bytes)
                     *   <SQRTSD/MINSD/...> XMM0  ; op nativa (~4-6 ciclos)
                     *   MOVQ_XMM_GP RAX, XMM0    ; rax = xmm0
                     *   store_op(dst, RAX)
                     *
                     * Compara vs CALLN: ~50ns + libm sqrt ~10ns = 60ns.
                     * Nativo: ~4-6 ciclos + 4 movs (~3ns total).  ~20x.
                     *
                     * XMM0/XMM1 hardcoded como scratch (regalloc D.7 no
                     * pina XMM, asi que es seguro).
                     * ================================================== */
                    case ir::IrOp::FADD:
                    case ir::IrOp::FSUB:
                    case ir::IrOp::FMUL:
                    case ir::IrOp::FDIV: {
                        /* FP binary: load both ops to XMM0/XMM1, op, store. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FADD/FSUB/FMUL/FDIV: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM1),
                                MOperand::make_reg(SCRATCH_B)));
                        const MOp mop =
                            (ins.op == ir::IrOp::FADD) ? MOp::ADDSD :
                            (ins.op == ir::IrOp::FSUB) ? MOp::SUBSD :
                            (ins.op == ir::IrOp::FMUL) ? MOp::MULSD :
                                                          MOp::DIVSD;
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(mop,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM1)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::ITOF:
                    case ir::IrOp::UITOF: {
                        /* CVTSI2SD xmm, gp.  ITOF interpreta signed; UITOF
                         * para u64 con bit 63 set requeriria correccion
                         * extra (no implementado en v1; rare). */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "ITOF/UITOF: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CVTSI2SD,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::FTOI:
                    case ir::IrOp::FTOUI: {
                        /* CVTTSD2SI gp, xmm.  Truncacion hacia cero. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FTOI/FTOUI: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CVTTSD2SI,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    case ir::IrOp::FSQRT: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FSQRT: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);  /* rcx = bits */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::SQRTSD,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM0)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::FMIN:
                    case ir::IrOp::FMAX: {
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FMIN/FMAX: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* op0 -> xmm0, op1 -> xmm1, minsd xmm0, xmm1. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM1),
                                MOperand::make_reg(SCRATCH_B)));
                        const MOp mop = (ins.op == ir::IrOp::FMIN)
                                          ? MOp::MINSD : MOp::MAXSD;
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(mop,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM1)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::FFLOOR:
                    case ir::IrOp::FCEIL:
                    case ir::IrOp::FROUND:
                    case ir::IrOp::FTRUNC: {
                        /* ROUNDSD xmm0, xmm0, imm8 (mode 0/1/2/3). */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FROUND family: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        const uint8_t mode =
                            (ins.op == ir::IrOp::FROUND) ? 0 :  /* nearest */
                            (ins.op == ir::IrOp::FFLOOR) ? 1 :  /* down */
                            (ins.op == ir::IrOp::FCEIL)  ? 2 :  /* up */
                                                            3;  /* trunc */
                        MInstr round_instr;
                        round_instr.op      = MOp::ROUNDSD;
                        round_instr.variant = mode;
                        round_instr.dst     = MOperand::make_reg(MReg::XMM0);
                        round_instr.src1    = MOperand::make_reg(MReg::XMM0);
                        mf.blocks.back().instrs.push_back(round_instr);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::FABS:
                    case ir::IrOp::FNEG: {
                        /* GP-only via bitwise mask en los bits IEEE 754:
                         *   FABS: AND con 0x7FFFFFFFFFFFFFFF (clear sign bit)
                         *   FNEG: XOR con 0x8000000000000000 (flip sign bit)
                         * Mas eficiente que XMM via SSE-mask (sin static_data).
                         * SCRATCH_A=RAX para la mask, SCRATCH_B=RCX para val. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FABS/FNEG: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        const uint64_t mask = (ins.op == ir::IrOp::FABS)
                            ? 0x7FFFFFFFFFFFFFFFULL
                            : 0x8000000000000000ULL;
                        /* mov rax, imm64 via pool. */
                        const uint32_t mask_idx = mf.intern_imm64(mask);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_imm64_idx(mask_idx)));
                        /* and rcx, rax  o  xor rcx, rax  (2-operand: dst = dst OP src). */
                        const MOp mop = (ins.op == ir::IrOp::FABS)
                                          ? MOp::AND : MOp::XOR;
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(mop,
                                MOperand::make_reg(SCRATCH_B),
                                MOperand::make_reg(MReg::RAX)));
                        store_op(mf, ins.dst, SCRATCH_B);
                        break;
                    }

                    case ir::IrOp::F32TOF64: {
                        /* f32 -> f64 widening via CVTSS2SD.  El operand en
                         * IR llega como GP (bits IEEE 754 f32 en low 32);
                         * pasar a XMM, ejecutar CVT, devolver bits f64 a GP. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "F32TOF64: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CVTSS2SD,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM0)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }
                    case ir::IrOp::F64TOF32: {
                        /* f64 -> f32 narrowing via CVTSD2SS.  Bits IEEE
                         * llegan como GP (f64), bajamos a XMM, CVT, devolvemos
                         * los bits low 32 (f32) a GP. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "F64TOF32: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::CVTSD2SS,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM0)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_XMM_GP,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::XMM0)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    case ir::IrOp::FCMP_EQ:
                    case ir::IrOp::FCMP_NE:
                    case ir::IrOp::FCMP_LT:
                    case ir::IrOp::FCMP_GT:
                    case ir::IrOp::FCMP_LE:
                    case ir::IrOp::FCMP_GE: {
                        /* Float compare: load both ops, UCOMISD, SETcc.
                         *
                         * UCOMISD setea ZF/PF/CF segun:
                         *   unordered (NaN): ZF=1, PF=1, CF=1
                         *   greater:         ZF=0, PF=0, CF=0
                         *   less:            ZF=0, PF=0, CF=1
                         *   equal:           ZF=1, PF=0, CF=0
                         *
                         * Cond codes para SETcc (con A,B,E,NE = unsigned-like):
                         *   ==     -> SETE       (ZF=1, ignora NaN como equal -- ojo)
                         *           Mejor: para evitar NaN==NaN=true, usar:
                         *             setnp tmp; sete dst; and dst, tmp
                         *   !=     -> SETNE | NaN
                         *           setp tmp; setne dst; or dst, tmp
                         *   <      -> SETB
                         *   >      -> SETA
                         *   <=     -> SETBE
                         *   >=     -> SETAE
                         *
                         * Para v1, omito el handling de NaN para EQ/NE
                         * (NaN==NaN dara true en EQ; comportamiento C-like
                         * sin -ffast-math).  El usuario que necesite NaN
                         * estricto puede usar `isnan(x) || ...`. */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FCMP_*: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* xor rax/rcx, ANTES de UCOMISD para no clobear
                         * las flags que setea (ZF/PF/CF).  Si los xor
                         * vinieran despues, la PF leida por SETNP/SETP
                         * seria la del XOR (result=0 -> PF=1), no de
                         * UCOMISD.  Bug capturado con bench_float_inf. */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::XOR,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::RAX)));
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::XOR,
                                MOperand::make_reg(MReg::RCX),
                                MOperand::make_reg(MReg::RCX)));
                        /* Cargar ambos operandos a XMM0/XMM1. */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(SCRATCH_B)));
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::MOVQ_GP_XMM,
                                MOperand::make_reg(MReg::XMM1),
                                MOperand::make_reg(SCRATCH_B)));
                        /* UCOMISD xmm0, xmm1 -> ZF/PF/CF.
                         *   unordered (NaN): ZF=1, PF=1, CF=1
                         *   greater:         ZF=0, PF=0, CF=0
                         *   less:            ZF=0, PF=0, CF=1
                         *   equal:           ZF=1, PF=0, CF=0  */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(MOp::UCOMISD,
                                MOperand::make_reg(MReg::XMM0),
                                MOperand::make_reg(MReg::XMM1)));

                        /* Sprint edge-bugs (2026-06-02): NaN-safe FCMP.
                         * UCOMISD setea ZF=1 cuando unordered (NaN), lo
                         * que confunde `SETE` (devuelve true) y rompe
                         * IEEE 754 (NaN == NaN debe ser false).
                         *
                         * Solucion: combinar el SETcc con la PF para
                         * distinguir ordered-equal de unordered:
                         *   EQ:  SETNP tmp; SETE dst; AND dst, tmp   (false si NaN)
                         *   NE:  SETP  tmp; SETNE dst; OR  dst, tmp  (true  si NaN)
                         *   LT/GT/LE/GE: usar SETB/SETA/SETBE/SETAE.
                         *     Esos cc devuelven false si NaN (CF=1+PF=1 -> SETB true).
                         *     Pero IEEE dice "ordered <" false si NaN.  Necesitamos
                         *     `SETB AND SETNP` para LT, `SETA AND SETNP` para GT, etc. */
                        const bool is_eq = (ins.op == ir::IrOp::FCMP_EQ);
                        const bool is_ne = (ins.op == ir::IrOp::FCMP_NE);
                        MCond cc;
                        switch (ins.op) {
                            case ir::IrOp::FCMP_EQ: cc = MCond::E;  break;
                            case ir::IrOp::FCMP_NE: cc = MCond::NE; break;
                            case ir::IrOp::FCMP_LT: cc = MCond::B;  break;
                            case ir::IrOp::FCMP_GT: cc = MCond::A;  break;
                            case ir::IrOp::FCMP_LE: cc = MCond::BE; break;
                            case ir::IrOp::FCMP_GE: cc = MCond::AE; break;
                            default: cc = MCond::E; break;
                        }
                        if (is_ne) {
                            /* RCX = SETP (PF=1 if unordered).
                             * RAX = SETNE.
                             * RAX = RAX | RCX  (true si NaN o NE). */
                            {
                                MInstr i;
                                i.op = MOp::SETCC;
                                i.variant = static_cast<uint8_t>(MCond::P);
                                i.dst = MOperand::make_reg(MReg::RCX);
                                mf.blocks.back().instrs.push_back(i);
                            }
                            {
                                MInstr i;
                                i.op = MOp::SETCC;
                                i.variant = static_cast<uint8_t>(MCond::NE);
                                i.dst = MOperand::make_reg(MReg::RAX);
                                mf.blocks.back().instrs.push_back(i);
                            }
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::OR,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_reg(MReg::RCX)));
                        } else {
                            /* EQ/LT/GT/LE/GE: enmascara con SETNP para
                             * ignorar NaN (cualquier comparacion ordered
                             * con NaN debe dar false). */
                            {
                                MInstr i;
                                i.op = MOp::SETCC;
                                i.variant = static_cast<uint8_t>(MCond::NP);
                                i.dst = MOperand::make_reg(MReg::RCX);
                                mf.blocks.back().instrs.push_back(i);
                            }
                            {
                                MInstr i;
                                i.op = MOp::SETCC;
                                i.variant = static_cast<uint8_t>(cc);
                                i.dst = MOperand::make_reg(MReg::RAX);
                                mf.blocks.back().instrs.push_back(i);
                            }
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::AND,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_reg(MReg::RCX)));
                            (void)is_eq;
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    case ir::IrOp::ROTL:
                    case ir::IrOp::ROTR: {
                        /* Sprint BBB: ROTL/ROTR nativo via rol/ror r64, cl.
                         * El encoder de SHL/SAR ya soporta la forma reg-cl
                         * (variante D3 /subop) cuando src es REG==RCX.
                         * Patron:
                         *   load val -> RAX
                         *   load cnt -> RCX (CL = low byte de RCX)
                         *   rol/ror rax, cl
                         *   store dst <- rax  */
                        if (ins.dst == ir::IR_NO_VALUE
                         || ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "ROTL/ROTR: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], SCRATCH_A);
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);
                        const MOp rot_op =
                            (ins.op == ir::IrOp::ROTL) ? MOp::ROL : MOp::ROR;
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_unary(rot_op,
                                MOperand::make_reg(SCRATCH_A),
                                MOperand::make_reg(SCRATCH_B)));
                        store_op(mf, ins.dst, SCRATCH_A);
                        break;
                    }

                    case ir::IrOp::RAW_ALLOC: {
                        /* RAW_ALLOC(size) -> host_ptr.  CALL a vrt_raw_alloc(proc, size).
                         * Equivale a @c malloc(size) del C runtime.
                         * El bloque vive hasta @c RAW_FREE manual. */
                        if (ins.operands.size() == 1
                         && ins.dst != ir::IR_NO_VALUE
                         && opts_.runtime != nullptr
                         && opts_.runtime->raw_alloc != nullptr) {
                            const uint64_t fn_addr = reinterpret_cast<uint64_t>(
                                opts_.runtime->raw_alloc);
#if defined(_WIN32)
                            const MReg arg0 = MReg::RCX;
                            const MReg arg1 = MReg::RDX;
#else
                            const MReg arg0 = MReg::RDI;
                            const MReg arg1 = MReg::RSI;
#endif
                            /* mov arg1, [slot[size]]. */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                            /* mov arg0, rbx (proc). */
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(arg0),
                                    MOperand::make_reg(MReg::RBX)));
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
                            store_op(mf, ins.dst, MReg::RAX);
                        } else {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->raw_alloc null o operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                        }
                        break;
                    }
                    case ir::IrOp::RAW_FREE: {
                        /* RAW_FREE(ptr) -> void.  CALL a vrt_raw_free(proc, ptr).
                         * Equivale a @c free(ptr) del C runtime. */
                        /* Sprint mem-loop-fix (2026-06-02): SKIP el free
                         * cuando el ptr proviene de un ALLOCA con
                         * host_alloca_explicit_free.  En el JIT esos
                         * ALLOCAs emiten `sub rsp, N` (host stack),
                         * que se libera con `leave/ret` del frame.
                         * Llamar `vrt_raw_free(proc, host_stack_ptr)`
                         * seria un crash (intentaria free sobre un
                         * ptr NO alocado por el RawAllocator).  El
                         * skip es solo para el JIT -- el INTERP SI
                         * ejecuta el `free` opcode porque ahi el
                         * ALLOCA si emitio `alloc` real. */
                        if (ins.operands.size() == 1
                         && ins.operands[0] != ir::IR_NO_VALUE
                         && ins.operands[0] < skip_raw_free_vid.size()
                         && skip_raw_free_vid[ins.operands[0]]) {
                            /* Sprint mem-loop-fix-v2: en lugar de SKIP,
                             * emite `add rsp, aligned(N)` para balancear
                             * el `sub rsp, N` del ALLOCA matching.  Asi
                             * loops con malloc(N)+...+free(p) no acumulan
                             * stack (480 MB en el bench mem_malloc_free
                             * antes del fix -> crash silente). */
                            const uint64_t size =
                                (ins.operands[0] < alloca_size_by_vid.size())
                                    ? alloca_size_by_vid[ins.operands[0]] : 0;
                            if (size > 0 && size < INT32_MAX) {
                                const uint64_t aligned =
                                    (size + 15ULL) & ~15ULL;
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_unary(MOp::ADD,
                                        MOperand::make_reg(MReg::RSP),
                                        MOperand::make_imm32(
                                            static_cast<int32_t>(aligned))));
                            }
                            break;
                        }
                        if (ins.operands.size() == 1
                         && opts_.runtime != nullptr
                         && opts_.runtime->raw_free != nullptr) {
                            const uint64_t fn_addr = reinterpret_cast<uint64_t>(
                                opts_.runtime->raw_free);
#if defined(_WIN32)
                            const MReg arg0 = MReg::RCX;
                            const MReg arg1 = MReg::RDX;
#else
                            const MReg arg0 = MReg::RDI;
                            const MReg arg1 = MReg::RSI;
#endif
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(arg0),
                                    MOperand::make_reg(MReg::RBX)));
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
                        } else {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->raw_free null o operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                        }
                        break;
                    }
                    case ir::IrOp::GC_DEREF_HOST: {
                        /* raw_asm-elim 2026-05-28: handle GC -> host_ptr.
                         * Reemplaza el blob RAW_ASM viejo con un CALL
                         * limpio a vrt_gc_deref(proc, handle).  Coste
                         * runtime: ~30 ns vs ~5 ns gcderef+xchg bytecode,
                         * pero gana cero patron-matching textual + stackmap
                         * automatico + compatibilidad con regalloc.
                         * C2 futuro puede inlinear el lookup. */
                        if (ins.operands.size() == 1
                         && ins.dst != ir::IR_NO_VALUE
                         && opts_.runtime != nullptr
                         && opts_.runtime->gc_deref != nullptr) {
                            const uint64_t fn_addr = reinterpret_cast<uint64_t>(
                                opts_.runtime->gc_deref);
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
                            /* mov arg1, [slot[handle]]. */
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                            /* mov rax, fn_addr (via imm64 pool). */
                            const uint32_t fn_pool_idx = mf.intern_imm64(fn_addr);
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_unary(MOp::MOV,
                                    MOperand::make_reg(MReg::RAX),
                                    MOperand::make_imm64_idx(fn_pool_idx)));
                            /* call rax + stackmap. */
                            MInstr call_instr;
                            call_instr.op = MOp::CALL;
                            call_instr.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(call_instr);
                            mf.blocks.back().instrs.push_back(call_instr);
                            /* host_ptr en RAX -> slot[dst]. */
                            store_op(mf, ins.dst, MReg::RAX);
                        } else {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->gc_deref null o operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                        }
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
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);

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
                                            /* src es VMreg, @Absolute(X) o imm. */
                                            int src_slot = vm_reg_slot_index(src);
                                            if (src_slot >= 0) {
                                                stage_load_slot(MReg::RAX, src_slot);
                                            } else if (!src.empty() && src[0] == '@'
                                                    && opts_.resolve_symbol) {
                                                /* @Absolute("X") / @StringRef("X"):
                                                 * resolver via symbol_table del linker
                                                 * y cargar la direccion como imm.  Usado
                                                 * por `mov {dst}, @Absolute("code.handler")`
                                                 * que el frontend emite en lower_try para
                                                 * el handler_pc de un catch block. */
                                                size_t lp = src.find('(');
                                                size_t rp = src.rfind(')');
                                                if (lp == std::string::npos
                                                 || rp == std::string::npos || rp <= lp + 1) {
                                                    all_ok = false; break;
                                                }
                                                std::string inner = src.substr(lp + 1, rp - lp - 1);
                                                if (inner.size() >= 2
                                                 && inner.front() == '"' && inner.back() == '"') {
                                                    inner = inner.substr(1, inner.size() - 2);
                                                }
                                                const uint64_t v = opts_.resolve_symbol(inner);
                                                if (v == 0) { all_ok = false; break; }
                                                stage_load_imm(MReg::RAX,
                                                    static_cast<int64_t>(v));
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
                                                            const uint64_t ic = opts_.reserve_ic_slot(0);
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
                                /* @c gchandle rN, rM -> vrt_gc_handle_for_ptr.
                                 * SKIP cuando args son placeholders {dst}/{srcN}:
                                 * el handler con placeholders esta abajo en la
                                 * nueva seccion (acepta gchandle como parte de
                                 * un bloque multilinea como synchronized prologue).
                                 * El check `args[i][0] != '{'` evita que esta
                                 * variante VMreg-only consuma el opcode y de
                                 * `all_ok=false` antes de llegar al handler con
                                 * placeholders. */
                                if (opcode == "gchandle" && args.size() == 2
                                 && opts_.runtime->gc_handle_for_ptr
                                 && !args[0].empty() && args[0][0] != '{'
                                 && !args[1].empty() && args[1][0] != '{') {
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

                                /* ==================================================
                                 * Helpers locales para SSA placeholders {dst}/{srcN}.
                                 * Reusados por tryenter/throw/monenter/monexit + mov.
                                 * Acepta: VMreg (rN/rsp/rbp), SSA placeholder ({dst},
                                 * {srcN}), @Absolute("X"), imm decimal/hex/binario.
                                 * ================================================== */
                                auto resolve_arg_to_reg =
                                    [&](const std::string &tok, MReg dst_reg) -> bool {
                                    /* {dst} placeholder. */
                                    if (tok == "{dst}") {
                                        if (ins.dst == ir::IR_NO_VALUE) return false;
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(dst_reg),
                                            slot_mem(ins.dst)));
                                        return true;
                                    }
                                    /* {srcN} placeholder. */
                                    if (tok.size() > 5
                                     && tok[0] == '{' && tok.back() == '}'
                                     && tok.compare(1, 3, "src") == 0) {
                                        int n = 0;
                                        for (size_t k = 4; k + 1 < tok.size(); ++k) {
                                            if (!std::isdigit(
                                                    static_cast<unsigned char>(tok[k]))) {
                                                return false;
                                            }
                                            n = n * 10 + (tok[k] - '0');
                                        }
                                        if (n < 0
                                         || static_cast<size_t>(n) >= ins.operands.size()) {
                                            return false;
                                        }
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(dst_reg),
                                            slot_mem(ins.operands[n])));
                                        return true;
                                    }
                                    /* @Absolute("X") / @StringRef("X") via resolve_symbol. */
                                    if (!tok.empty() && tok[0] == '@'
                                     && opts_.resolve_symbol) {
                                        size_t lp = tok.find('(');
                                        size_t rp = tok.rfind(')');
                                        if (lp != std::string::npos && rp != std::string::npos
                                         && rp > lp + 1) {
                                            std::string inner = tok.substr(lp + 1, rp - lp - 1);
                                            if (inner.size() >= 2
                                             && inner.front() == '"' && inner.back() == '"') {
                                                inner = inner.substr(1, inner.size() - 2);
                                            }
                                            const uint64_t v = opts_.resolve_symbol(inner);
                                            if (v != 0) {
                                                stage_load_imm(dst_reg,
                                                    static_cast<int64_t>(v));
                                                return true;
                                            }
                                        }
                                        return false;
                                    }
                                    /* VMreg fallback (rN). */
                                    const int slot = vm_reg_slot_index(tok);
                                    if (slot >= 0) {
                                        stage_load_slot(dst_reg, slot);
                                        return true;
                                    }
                                    /* Imm fallback. */
                                    int64_t imm;
                                    if (parse_imm_int(tok, imm)) {
                                        stage_load_imm(dst_reg, imm);
                                        return true;
                                    }
                                    return false;
                                };

                                /* Helper para almacenar RAX al destino (VMreg/SSA placeholder). */
                                auto store_rax_to_dst =
                                    [&](const std::string &tok) -> bool {
                                    if (tok == "{dst}") {
                                        if (ins.dst == ir::IR_NO_VALUE) return false;
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            slot_mem(ins.dst),
                                            MOperand::make_reg(MReg::RAX)));
                                        return true;
                                    }
                                    if (tok.size() > 5
                                     && tok[0] == '{' && tok.back() == '}'
                                     && tok.compare(1, 3, "src") == 0) {
                                        int n = 0;
                                        for (size_t k = 4; k + 1 < tok.size(); ++k) {
                                            if (!std::isdigit(
                                                    static_cast<unsigned char>(tok[k]))) {
                                                return false;
                                            }
                                            n = n * 10 + (tok[k] - '0');
                                        }
                                        if (n < 0
                                         || static_cast<size_t>(n) >= ins.operands.size()) {
                                            return false;
                                        }
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            slot_mem(ins.operands[n]),
                                            MOperand::make_reg(MReg::RAX)));
                                        return true;
                                    }
                                    const int slot = vm_reg_slot_index(tok);
                                    if (slot >= 0) {
                                        stage_store_slot(slot, MReg::RAX);
                                        return true;
                                    }
                                    return false;
                                };

                                /* ==================================================
                                 * mov {dst}, @Absolute("code.X") -- emitido por try
                                 * para cargar handler_pc en un SSA value.
                                 * ==================================================
                                 * Resolve @Absolute via resolve_symbol y store imm
                                 * al slot SSA del {dst}.  2 instr x86-64 (mov imm64
                                 * + mov [rbp - off], rax). */
                                if (opcode == "mov" && args.size() == 2
                                 && args[0] == "{dst}"
                                 && !args[1].empty() && args[1][0] == '@'
                                 && opts_.resolve_symbol) {
                                    if (!resolve_arg_to_reg(args[1], MReg::RAX)) {
                                        all_ok = false; break;
                                    }
                                    if (!store_rax_to_dst(args[0])) {
                                        all_ok = false; break;
                                    }
                                    continue;
                                }

                                /* ==================================================
                                 * gchandle {dst}, {src0} (con placeholders SSA)
                                 * ==================================================
                                 * Variante del handler `gchandle rN, rM` ya existente
                                 * que acepta {dst}/{srcN} en lugar de VMregs.  Usado
                                 * cuando el frontend Vex emite la version multi-linea
                                 * (synchronized prologue: comment + gchandle + monenter).
                                 *
                                 * vrt_gc_handle_for_ptr(proc, host_ptr) -> handle. */
                                if (opcode == "gchandle" && args.size() == 2
                                 && opts_.runtime->gc_handle_for_ptr
                                 && (args[0].size() > 0 && args[0][0] == '{'
                                  || args[1].size() > 0 && args[1][0] == '{')) {
                                    if (!resolve_arg_to_reg(args[1], ABI_ARG1)) {
                                        all_ok = false; break;
                                    }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(
                                        opts_.runtime->gc_handle_for_ptr));
                                    if (!store_rax_to_dst(args[0])) {
                                        all_ok = false; break;
                                    }
                                    continue;
                                }

                                /* ==================================================
                                 * tryleave (sin args) -- INLINE 5 instrucciones
                                 * ==================================================
                                 *
                                 * Inline pop de exc_frame_stack como linked list.
                                 * El frame popped queda leaked en heap raw (~176 B,
                                 * aceptable v1; documentado en SelectorOptions).
                                 *
                                 *   mov rax, [rbx + EXC_FRAME_OFF]   ; top
                                 *   test rax, rax
                                 *   je skip
                                 *   mov rcx, [rax + 168]              ; prev
                                 *   mov [rbx + EXC_FRAME_OFF], rcx    ; head = prev
                                 * skip:
                                 *
                                 * 5 instr ~5-8 ciclos vs ~25 ciclos del runtime call.
                                 * Si exc_frame_stack_offset==0 (no configurado), fallback
                                 * a vrt_tryleave call (~10 instr). */
                                if (opcode == "tryleave" && args.empty()) {
                                    if (opts_.exc_frame_stack_offset != 0) {
                                        const uint32_t skip_lbl = mf.new_label();
                                        const int32_t exc_off = opts_.exc_frame_stack_offset;
                                        const int32_t free_off = opts_.exc_free_list_offset;
                                        /* mov rax, [rbx + exc_off]    ; top */
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(MReg::RAX),
                                            MOperand::make_mem(MReg::RBX, exc_off)));
                                        /* test rax, rax */
                                        staged.push_back(MInstr::make_unary(MOp::TEST,
                                            MOperand::make_reg(MReg::RAX),
                                            MOperand::make_reg(MReg::RAX)));
                                        /* je skip */
                                        staged.push_back(MInstr::make_jcc(MCond::E, skip_lbl));
                                        /* mov rcx, [rax + 168]        ; prev */
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_reg(MReg::RCX),
                                            MOperand::make_mem(MReg::RAX,
                                                VESTA_EXC_FRAME_PREV_OFFSET)));
                                        /* mov [rbx + exc_off], rcx    ; head = prev */
                                        staged.push_back(MInstr::make_unary(MOp::MOV,
                                            MOperand::make_mem(MReg::RBX, exc_off),
                                            MOperand::make_reg(MReg::RCX)));
                                        /* Si tenemos free_list_offset, push al free
                                         * list para reciclar el frame (no leak).
                                         * Coste: 2 instrucciones extra. */
                                        if (free_off != 0) {
                                            /* mov rcx, [rbx + free_off]   ; old free head */
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(MReg::RCX),
                                                MOperand::make_mem(MReg::RBX, free_off)));
                                            /* mov [rax + 168], rcx        ; popped.prev = old head */
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_mem(MReg::RAX,
                                                    VESTA_EXC_FRAME_PREV_OFFSET),
                                                MOperand::make_reg(MReg::RCX)));
                                            /* mov [rbx + free_off], rax   ; free head = popped */
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_mem(MReg::RBX, free_off),
                                                MOperand::make_reg(MReg::RAX)));
                                        }
                                        /* skip: */
                                        staged.push_back(MInstr::make_label_def(skip_lbl));
                                        continue;
                                    }
                                    /* Fallback: runtime call. */
                                    if (!opts_.runtime->tryleave) { all_ok = false; break; }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(
                                        opts_.runtime->tryleave));
                                    continue;
                                }

                                /* ==================================================
                                 * tryenter {handler_pc}, {type} -> vrt_tryenter
                                 * ==================================================
                                 * Calling convention C: (proc, handler_pc, type_class).
                                 * El frontend emite `tryenter {src0}, {src1}` con
                                 * src0=handler_pc y src1=ClassInfo* (o 0).
                                 *
                                 * El runtime call es OBLIGATORIO aqui: alocar y
                                 * popular un ExceptionFrame de 176 bytes + snapshot
                                 * de 16 regs requiere malloc + 22 stores; inlinear
                                 * eso seria mas de 30 MInstrs cuando un call es ~3. */
                                if (opcode == "tryenter" && args.size() == 2
                                 && opts_.runtime->tryenter) {
                                    if (!resolve_arg_to_reg(args[0], ABI_ARG1)) {
                                        all_ok = false; break;
                                    }
                                    if (!resolve_arg_to_reg(args[1], ABI_ARG2)) {
                                        all_ok = false; break;
                                    }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(
                                        opts_.runtime->tryenter));
                                    continue;
                                }

                                /* Helper para emitir el epilogue de la funcion JIT
                                 * tras throw/rethrow.  Las funciones runtime do_throw
                                 * actualizan proc->rip al handler_pc pero NO hacen
                                 * longjmp; retornan normalmente.  Sin el epilogue
                                 * inline aqui, el codigo JIT continuaria con la
                                 * siguiente instr en host (basura/fallthrough).
                                 * Emitiendo el epilogue, el JIT retorna a
                                 * @c enter_jit -> exec_instr_callvirt -> scheduler,
                                 * y el scheduler dispatcha el nuevo proc->rip
                                 * (el catch handler en bytecode interp).
                                 *
                                 * Mismo epilogue que IrOp::RET (sin la parte de
                                 * cargar return value, porque do_throw ya seteo R0
                                 * con el exc handle).  4-7 instrucciones segun
                                 * regalloc.callee_saved_used + modo VM_ABI. */
                                auto stage_function_epilogue = [&]() {
                                    /* callback-ABI safe: restaurar el banco VM
                                     * salvado antes de desmontar el frame (rbx
                                     * y rbp aun validos aqui).  SCRATCH_B=rcx. */
                                    if (cb_entry && cb_save_all) {
                                        for (uint32_t r = 0; r < 16; ++r) {
                                            const int32_t reg_off = VESTA_PROC_REGISTERS_OFFSET
                                                + static_cast<int32_t>(r) * VESTA_REGISTER_SIZE;
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_reg(SCRATCH_B),
                                                MOperand::make_mem(MReg::RBP, cb_save_off(r))));
                                            staged.push_back(MInstr::make_unary(MOp::MOV,
                                                MOperand::make_mem(MReg::RBX, reg_off),
                                                MOperand::make_reg(SCRATCH_B)));
                                        }
                                    }
                                    /* mov rsp, rbp */
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(MReg::RSP),
                                        MOperand::make_reg(MReg::RBP)));
                                    /* pop rbp */
                                    staged.push_back(MInstr::make_unary(MOp::POP,
                                        MOperand::make_reg(MReg::RBP), {}));
                                    /* pop regs callee-saved en orden inverso. */
                                    for (auto it = regalloc.callee_saved_used.rbegin();
                                         it != regalloc.callee_saved_used.rend(); ++it) {
                                        staged.push_back(MInstr::make_unary(MOp::POP,
                                            MOperand::make_reg(*it), {}));
                                    }
                                    if (opts_.mode == SelectorMode::VM_ABI) {
                                        /* pop rbx (proc se preservaba aqui). */
                                        staged.push_back(MInstr::make_unary(MOp::POP,
                                            MOperand::make_reg(MReg::RBX), {}));
                                    }
                                    /* ret */
                                    staged.push_back(MInstr::make_ret());
                                };

                                /* ==================================================
                                 * throw {src0} -> vrt_throw_user(proc, handle) + epilogue
                                 * ==================================================
                                 * 1. Pasa el handle como arg1, proc como arg0.
                                 * 2. CALL vrt_throw_user (do_throw modifica proc->rip).
                                 * 3. Emite el epilogue de la funcion JIT para que
                                 *    retorne a enter_jit/scheduler.  Scheduler ve
                                 *    proc->rip cambiado y dispatcha el catch handler. */
                                if (opcode == "throw" && args.size() == 1
                                 && opts_.runtime->throw_user) {
                                    if (!resolve_arg_to_reg(args[0], ABI_ARG1)) {
                                        all_ok = false; break;
                                    }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(
                                        opts_.runtime->throw_user));
                                    stage_function_epilogue();
                                    continue;
                                }

                                /* ==================================================
                                 * rethrow (sin args) -> vrt_rethrow(proc) + epilogue
                                 * ==================================================
                                 * Relanza proc->current_exception.  No retorna al JIT;
                                 * el epilogue garantiza control transferido al scheduler. */
                                if (opcode == "rethrow" && args.empty()
                                 && opts_.runtime->rethrow) {
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(reinterpret_cast<uint64_t>(
                                        opts_.runtime->rethrow));
                                    stage_function_epilogue();
                                    continue;
                                }

                                /* ==================================================
                                 * monenter {handle} -> vrt_monitor_enter(proc, handle)
                                 * monexit  {handle} -> vrt_monitor_exit(proc, handle)
                                 * ==================================================
                                 * El frontend emite ambos con un SSA value (a veces
                                 * {dst} de un gchandle inmediato anterior).  Calling
                                 * convention C: (proc, handle as u32->u64).
                                 *
                                 * Fast path del runtime: CAS sobre ObjectHeader,
                                 * ~10 ns sin contencion.  Slow path: scheduler
                                 * coordina blocking.  No inlineable sin exponer
                                 * GcHeap layout completo (HandleTable + ObjectHeader).
                                 * Para v1 runtime call es lo correcto. */
                                if ((opcode == "monenter" || opcode == "monexit")
                                 && args.size() == 1) {
                                    const bool is_enter = (opcode == "monenter");
                                    const uint64_t fn_addr = is_enter
                                        ? reinterpret_cast<uint64_t>(
                                              opts_.runtime->monitor_enter)
                                        : reinterpret_cast<uint64_t>(
                                              opts_.runtime->monitor_exit);
                                    if (fn_addr == 0) { all_ok = false; break; }
                                    if (!resolve_arg_to_reg(args[0], ABI_ARG1)) {
                                        all_ok = false; break;
                                    }
                                    staged.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(ABI_ARG0),
                                        MOperand::make_reg(MReg::RBX)));
                                    stage_call(fn_addr);
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

                    /* ==================================================
                     * Sprint JIT-cobertura (2026-06-01): IR ops anyadidos
                     * para subir cobertura del JIT del 63% al ~85%+.
                     *
                     * Patron comun: CALL nativo a vrt_* con marshalling
                     * Native ABI (Win64 RCX/RDX/R8/R9 o SysV RDI/RSI/RDX/RCX)
                     * + stackmap automatico en el call site para precise GC.
                     * ================================================== */

                    /* FINDCLASS: %dst = ClassInfo* por nombre (lookup en
                     * ClassRegistry).  Patron: CALL vrt_findclass(proc, params).
                     * Mismo shape que FINDMETHOD/FINDFIELD. */
                    case ir::IrOp::FINDCLASS: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "FINDCLASS: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->findclass) : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->findclass no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* GC_HANDLE_FOR_PTR: %dst = GcHandle (uint32 zero-ext) a
                     * partir de host_ptr al payload.  CALL vrt_gc_handle_for_ptr. */
                    case ir::IrOp::GC_HANDLE_FOR_PTR: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "GC_HANDLE_FOR_PTR: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->gc_handle_for_ptr) : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->gc_handle_for_ptr no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* TRYENTER: push ExceptionFrame{handler_pc, type}.
                     * CALL vrt_tryenter(proc, handler_pc, type_class).
                     * No produce dst. */
                    case ir::IrOp::TRYENTER: {
                        if (ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "TRYENTER: requiere 2 operandos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->tryenter) : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->tryenter no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX, arg2 = MReg::R8;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI, arg2 = MReg::RDX;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);  /* handler_pc */
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], arg2);  /* type_class */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        break;
                    }

                    /* TRYLEAVE: pop top ExceptionFrame. CALL vrt_tryleave(proc). */
                    case ir::IrOp::TRYLEAVE: {
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->tryleave) : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->tryleave no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX;
                    #else
                        const MReg arg0 = MReg::RDI;
                    #endif
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        break;
                    }

                    /* GETSTATIC: %dst = i64 desde cls->static_data + offset.
                     * Acceso DIRECTO a memoria host (sin runtime call):
                     *   mov rax, [cls_slot]                                ; rax = ClassInfo*
                     *   mov rax, [rax + VESTA_CLASSINFO_STATIC_DATA_OFFSET] ; rax = static_data ptr
                     *   mov rax, [rax + imm32_offset]                       ; rax = i64 value
                     *   store_op(dst, rax)
                     *
                     * ~3 instr vs ~50ns de CALL runtime entry. */
                    case ir::IrOp::GETSTATIC: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "GETSTATIC: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                        /* rax = cls->static_data (host_ptr) */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RAX, VESTA_CLASSINFO_STATIC_DATA_OFFSET)));
                        /* rax = *(rax + offset)  -- imm caps a int32 */
                        const int32_t off = static_cast<int32_t>(ins.imm);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RAX, off)));
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* ISNULL: %dst = (src == 0) ? 1 : 0.  Emit:
                     *   load src -> rax
                     *   xor rcx, rcx          ; rcx = 0
                     *   test rax, rax         ; ZF=1 si rax==0
                     *   sete cl               ; cl = 1 si ZF
                     *   store dst <- rcx
                     */
                    case ir::IrOp::ISNULL: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "ISNULL: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                        /* xor rcx, rcx (clear destino antes del setcc para
                         * zero-extend los bits altos; setcc solo escribe AL) */
                        mf.blocks.back().instrs.push_back(MInstr::make_binary(MOp::XOR,
                            MOperand::make_reg(SCRATCH_B),
                            MOperand::make_reg(SCRATCH_B), {}));
                        mf.blocks.back().instrs.push_back(MInstr::make_binary(MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX), {}));
                        {
                            MInstr setcc{};
                            setcc.op = MOp::SETCC;
                            setcc.variant = static_cast<uint8_t>(MCond::E);
                            setcc.dst = MOperand::make_reg(SCRATCH_B, 1);
                            mf.blocks.back().instrs.push_back(setcc);
                        }
                        store_op(mf, ins.dst, SCRATCH_B);
                        break;
                    }

                    /* THROW: lanza una excepcion user-defined.  CALL
                     * vrt_throw_user(proc, exc_handle).  Nunca retorna
                     * normalmente (longjmp al handler de tryenter).
                     * No produce dst. */
                    case ir::IrOp::THROW: {
                        if (ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "THROW: requiere operand 0 (exception handle)");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->throw_user) : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->throw_user no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        /* Marca block como terminado: throw nunca retorna. */
                        break;
                    }

                    /* PANIC: lanza FatalError(USER_ABORT, msg).
                     *
                     * BugFix JIT-exc (2026-06-06): se BAILA a interp.  El path
                     * JIT de panic (CALL vrt_panic_str -> throw_fatal) requiere
                     * desenrollar el frame JIT-eado durante el unwind hacia el
                     * catch del caller; cuando el caller es interp (o cruza la
                     * frontera JIT<->interp) ese unwind tiene un crash flaky
                     * (~20% de runs, no determinista) por limpieza incorrecta
                     * del frame.  panic() es un error path (NUNCA hot), asi que
                     * forzar la funcion a interp -- donde el unwind via
                     * setjmp/longjmp es robusto -- no cuesta performance y
                     * elimina el crash.  El unwind nativo de frames JIT-eados
                     * es trabajo de Phase G (.pdata/.eh_frame); hasta entonces
                     * las funciones que lanzan se quedan en interp. */
                    case ir::IrOp::PANIC: {
                        warn_unsupported(ins.op, ins.source_line,
                            "PANIC: bail a interp (unwind JIT->catch no robusto)");
                        unsupported = true;
                        mf.blocks.back().instrs.push_back(
                            {MOp::INT3, 0, 0, 0, {}, {}, {}});
                        break;
                    }

                    /* GC_ALLOC: %dst = vrt_gc_alloc(proc, size) -- handle.
                     * GC_ALLOCP: %dst = vrt_gc_alloc_payload(proc, size) -- host_ptr.
                     * Ambos toman size en operand 0 (CONST i64 tipico). */
                    case ir::IrOp::GC_ALLOC:
                    case ir::IrOp::GC_ALLOCP: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "GC_ALLOC/GC_ALLOCP: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* BUG FIX (2026-06-05): GC_ALLOC y GC_ALLOCP bajan AMBOS
                         * a `gcallocp` en el ir_emitter -> host_ptr al PAYLOAD.
                         * El mapeo previo GC_ALLOC->gc_alloc devolvia un HANDLE
                         * (uint32) que el IR usaba como host_ptr -> store a un
                         * valor pequeno (1,2,..) = segfault (64_curry/102/167).
                         * Ambos deben usar gc_alloc_payload. */
                        const uint64_t fn_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->gc_alloc_payload)
                            : 0;
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->gc_alloc[/_payload] no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);  /* size */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* MONENTER/MONEXIT/MONWAIT/MONNOTI/MONNOTA: monitor ops.
                     * CALL vrt_monitor_*(proc, obj_handle).  No producen dst
                     * (excepto el lock_depth de monitor_enter pero es opaque).
                     * Convencion: el IR pasa el GcHandle del object como
                     * operands[0] (uint32 zero-ext en el slot).
                     *
                     * Multiples ops comparten el mismo shape de marshalling. */
                    case ir::IrOp::MONENTER:
                    case ir::IrOp::MONEXIT:
                    case ir::IrOp::MONWAIT:
                    case ir::IrOp::MONNOTI:
                    case ir::IrOp::MONNOTA: {
                        if (ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "MONITOR op: requiere operand 0 (obj handle)");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        uint64_t fn_addr = 0;
                        if (opts_.runtime) {
                            switch (ins.op) {
                                case ir::IrOp::MONENTER:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->monitor_enter); break;
                                case ir::IrOp::MONEXIT:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->monitor_exit); break;
                                case ir::IrOp::MONWAIT:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->monitor_wait); break;
                                case ir::IrOp::MONNOTI:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->monitor_notify); break;
                                case ir::IrOp::MONNOTA:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->monitor_notify_all); break;
                                default: break;
                            }
                        }
                        if (fn_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->monitor_* no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI;
                    #endif
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        /* MONENTER devuelve int32 (1 OK / 0 fail) en RAX --
                         * pero el IR no captura el dst (semanticamente es
                         * void).  No hacemos store_op. */
                        break;
                    }

                    /* STRMAKE/STRLEN/STRGETBYTES/STRRAW/STRCAT/STRCMP:
                     * delegan a vrt_str_* con marshalling Native ABI.
                     *
                     * STRMAKE:    dst = vrt_str_make(proc, vm_addr, byte_len)
                     * STRLEN:     dst = vrt_str_len(proc, str_handle)
                     * STRGETBYTES: dst = vrt_str_get_bytes(proc, str_handle)
                     * STRRAW:     dst = vrt_str_raw(proc, str_handle)  -> host_ptr
                     * STRCAT:     dst = vrt_str_cat(proc, a, b)
                     * STRCMP:     dst = vrt_str_cmp(proc, a, b)
                     */
                    case ir::IrOp::STRMAKE:
                    case ir::IrOp::STRLEN:
                    case ir::IrOp::STRGETBYTES:
                    case ir::IrOp::STRRAW:
                    case ir::IrOp::STRCAT:
                    case ir::IrOp::STRCMP: {
                        if (ins.dst == ir::IR_NO_VALUE) {
                            warn_unsupported(ins.op, ins.source_line,
                                "STR* op: dst invalido");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        uint64_t fn_addr = 0;
                        size_t nargs = 0;
                        if (opts_.runtime) {
                            switch (ins.op) {
                                case ir::IrOp::STRMAKE:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_make);
                                    nargs = 2; break;
                                case ir::IrOp::STRLEN:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_len);
                                    nargs = 1; break;
                                case ir::IrOp::STRGETBYTES:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_get_bytes);
                                    nargs = 1; break;
                                case ir::IrOp::STRRAW:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_raw);
                                    nargs = 1; break;
                                case ir::IrOp::STRCAT:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_cat);
                                    nargs = 2; break;
                                case ir::IrOp::STRCMP:
                                    fn_addr = reinterpret_cast<uint64_t>(opts_.runtime->str_cmp);
                                    nargs = 2; break;
                                default: break;
                            }
                        }
                        if (fn_addr == 0 || ins.operands.size() < nargs) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->str_* no resuelto u operandos insuficientes");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                    #if defined(_WIN32)
                        const MReg arg0 = MReg::RCX, arg1 = MReg::RDX, arg2 = MReg::R8;
                    #else
                        const MReg arg0 = MReg::RDI, arg1 = MReg::RSI, arg2 = MReg::RDX;
                    #endif
                        if (nargs >= 1) load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                        if (nargs >= 2) load_op_rematerializable(mf, ir_fn, ins.operands[1], arg2);

                        // Sprint string-perf-2 (2026-06-02): inline cache
                        // per call site para STRMAKE.  Layout del slot (24 B):
                        //   +0..+7   cached_arg1 (vm_addr o ha)
                        //   +8..+15  cached_arg2 (byte_len o hb; 0 si nargs==1)
                        //   +16..+23 cached_result (GcHandle; 0 = miss)
                        //
                        // En cada call site, check si args matchean cache.
                        // Hit: ~6 instr (~5 ns).  Miss: full CALL + actualizar
                        // slot (~250 ns).  Para STRMAKE/STRCAT con args
                        // repetidos en hot loops (literales, prefix-suffix
                        // recurrente) la ganancia es 50x.  LICM ya hoista la
                        // mayoria pero esto cubre los casos cross-function +
                        // patrones que LICM no atrapa.
                        //
                        // Solo aplica a STRMAKE/STRCAT/STRCMP (ops puros con
                        // resultado determinista por args).  STRLEN/STRRAW/
                        // STRGETBYTES tienen su propio fast path en el
                        // runtime (header read directo).
                        // Sprint string-perf-2 bug fix (2026-06-02): STRMAKE
                        // EXCLUIDO del JIT IC.  Su vm_addr es una direccion
                        // de memoria runtime, NO un value-deterministic
                        // handle.  Cachear por vm_addr es unsafe cuando los
                        // bytes en esa direccion mutan (caso comun: `&local`
                        // o `&buf[i]` con stores intermedios).
                        //
                        // STRCAT/STRCMP SI son safe: sus args son GcHandles
                        // (canonicos via intern), asi mismos handles ->
                        // mismo resultado deterministico.  IC por handle es
                        // semanticamente correcto.
                        //
                        // El runtime intern hash cache (hash de contenido)
                        // ya cubre las STRMAKEs de literales -- ~50ns hit.
                        // El JIT IC anyadia ~5ns vs ~50ns, pero a costo de
                        // correctness.  Diferencia despreciable.
                        // Sprint string-perf-2 debug (2026-06-02): IC
                        // deshabilitado temporalmente para validar
                        // correctness en bench string_workout.
                        // TODO: re-enable tras determinar causa raiz
                        // de discrepancia interp vs JIT.
                        bool use_ic = false;
                        (void)opts_.reserve_ic_slot;
                        uint64_t str_ic_slot = 0;
                        MLabelId ic_hit_label = 0, ic_miss_label = 0, ic_done_label = 0;
                        if (use_ic) {
                            str_ic_slot = opts_.reserve_ic_slot(0);
                            use_ic = (str_ic_slot != 0);
                        }
                        if (use_ic) {
                            ic_hit_label  = mf.new_label();
                            ic_miss_label = mf.new_label();
                            ic_done_label = mf.new_label();
                            // R10 = ic_slot_addr
                            const uint32_t slot_idx = mf.intern_imm64(str_ic_slot);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R10),
                                MOperand::make_imm64_idx(slot_idx)));
                            // cmp arg1, [r10+0]; jne miss
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::CMP,
                                MOperand::make_reg(arg1),
                                MOperand::make_mem(MReg::R10, 0)));
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_jcc(MCond::NE, ic_miss_label));
                            if (nargs >= 2) {
                                mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::CMP,
                                    MOperand::make_reg(arg2),
                                    MOperand::make_mem(MReg::R10, 8)));
                                mf.blocks.back().instrs.push_back(
                                    MInstr::make_jcc(MCond::NE, ic_miss_label));
                            }
                            // mov rax, [r10+16]; test rax,rax; je miss (handle 0 = empty)
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_mem(MReg::R10, 16)));
                            mf.blocks.back().instrs.push_back(MInstr::make_binary(
                                MOp::TEST,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::RAX)));
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_jcc(MCond::E, ic_miss_label));
                            // hit: jmp done
                            mf.blocks.back().instrs.push_back(MInstr::make_jmp(ic_done_label));
                            // miss:
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_label_def(ic_miss_label));
                        }

                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(arg0), MOperand::make_reg(MReg::RBX)));
                        const uint32_t fn_idx = mf.intern_imm64(fn_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        if (use_ic) {
                            // Tras CALL, RAX = nuevo handle.  Persistir args
                            // + handle en slot (R10 puede haber sido clobbered
                            // por el CALL: re-cargar).  Re-load args desde
                            // su slot original via load_op (idempotente).
                            const uint32_t slot_idx2 = mf.intern_imm64(str_ic_slot);
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R10),
                                MOperand::make_imm64_idx(slot_idx2)));
                            // Save rax to a temp (will be clobbered by load_op).
                            // Use R11 (caller-saved scratch, libre).
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::R11), MOperand::make_reg(MReg::RAX)));
                            // Re-load args from their SSA slots back to arg1/arg2.
                            load_op_rematerializable(mf, ir_fn, ins.operands[0], arg1);
                            if (nargs >= 2) load_op_rematerializable(mf, ir_fn, ins.operands[1], arg2);
                            // mov [r10+0], arg1
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::R10, 0),
                                MOperand::make_reg(arg1)));
                            // mov [r10+8], arg2 (or 0 if nargs==1)
                            if (nargs >= 2) {
                                mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                    MOperand::make_mem(MReg::R10, 8),
                                    MOperand::make_reg(arg2)));
                            }
                            // mov [r10+16], r11 (cached handle)
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_mem(MReg::R10, 16),
                                MOperand::make_reg(MReg::R11)));
                            // mov rax, r11 (restore result for store_op)
                            mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(MReg::RAX),
                                MOperand::make_reg(MReg::R11)));
                            // done:
                            mf.blocks.back().instrs.push_back(
                                MInstr::make_label_def(ic_done_label));
                        }
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* UNWRAP: %dst = unwrap %src.  Si src == 0, lanza
                     * NullPointerException (FATAL_NULL_POINTER capturable);
                     * si no, dst = src.  Patron equivalente al opcode
                     * bytecode unwrap.
                     *
                     * Emit:
                     *   mov rax, [src_slot]
                     *   test rax, rax
                     *   jne ok
                     *   mov rcx, proc; mov rdx, FATAL_NULL_POINTER;
                     *   mov r8, msg_ptr (literal "unwrap on null"); call throw_fatal
                     *   ok:
                     *   store dst <- rax
                     *
                     * Para simplicidad v1: NO emit el label + branch.  En su
                     * lugar, llamar a un helper inline vrt_unwrap que hace
                     * el null check + throw.  Eso es 1 call vs ~6 instrs y
                     * mantiene el codigo del JIT mas compacto.  Como vrt_unwrap
                     * no existe, fallback a una secuencia simple:
                     *   load src -> rax
                     *   test rax, rax
                     *   jne ok
                     *   <call throw_fatal>
                     *   ok: store dst <- rax
                     */
                    case ir::IrOp::UNWRAP: {
                        if (ins.dst == ir::IR_NO_VALUE || ins.operands.empty()) {
                            warn_unsupported(ins.op, ins.source_line,
                                "UNWRAP: operandos invalidos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        const uint64_t throw_addr = opts_.runtime
                            ? reinterpret_cast<uint64_t>(opts_.runtime->throw_fatal) : 0;
                        if (throw_addr == 0) {
                            warn_unsupported(ins.op, ins.source_line,
                                "runtime->throw_fatal no resuelto");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        /* load src -> RAX */
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);
                        /* test rax, rax (sets ZF=1 si rax==0) */
                        mf.blocks.back().instrs.push_back(MInstr::make_binary(
                            MOp::TEST,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_reg(MReg::RAX), {}));
                        /* JNE ok (saltar al store_op si != 0).  Asignamos
                         * un nuevo label local a este block. */
                        const MLabelId ok_lbl = mf.new_label();
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_jcc(MCond::NE, ok_lbl));
                        /* throw_fatal(proc, FATAL_NULL_POINTER=1, msg_ptr_0).
                         * msg=0 es valido (throw_fatal default a "unwrap"). */
                    #if defined(_WIN32)
                        const MReg targ0 = MReg::RCX, targ1 = MReg::RDX, targ2 = MReg::R8;
                    #else
                        const MReg targ0 = MReg::RDI, targ1 = MReg::RSI, targ2 = MReg::RDX;
                    #endif
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(targ0), MOperand::make_reg(MReg::RBX)));
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(targ1),
                            MOperand::make_imm32(static_cast<int32_t>(VESTA_FATAL_NULL_POINTER))));
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(targ2),
                            MOperand::make_imm32(0)));
                        const uint32_t fn_idx = mf.intern_imm64(throw_addr);
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_imm64_idx(fn_idx)));
                        {
                            MInstr ic;
                            ic.op = MOp::CALL;
                            ic.src1 = MOperand::make_reg(MReg::RAX);
                            emit_stackmap_for_safepoint(ic);
                            mf.blocks.back().instrs.push_back(ic);
                        }
                        /* ok: label */
                        mf.blocks.back().instrs.push_back(
                            MInstr::make_label_def(ok_lbl));
                        /* Tras el branch, RAX (cargado al principio) es el
                         * valor unwrapped.  Store al dst. */
                        store_op(mf, ins.dst, MReg::RAX);
                        break;
                    }

                    /* SETSTATIC: cls->static_data + offset = val.
                     *   mov rax, [cls_slot]
                     *   mov rax, [rax + STATIC_DATA_OFFSET]
                     *   mov rcx, [val_slot]
                     *   mov [rax + offset], rcx
                     */
                    case ir::IrOp::SETSTATIC: {
                        if (ins.operands.size() < 2) {
                            warn_unsupported(ins.op, ins.source_line,
                                "SETSTATIC: requiere 2 operandos");
                            unsupported = true;
                            mf.blocks.back().instrs.push_back(
                                {MOp::INT3, 0, 0, 0, {}, {}, {}});
                            break;
                        }
                        load_op_rematerializable(mf, ir_fn, ins.operands[0], MReg::RAX);  /* cls */
                        mf.blocks.back().instrs.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(MReg::RAX),
                            MOperand::make_mem(MReg::RAX, VESTA_CLASSINFO_STATIC_DATA_OFFSET)));
                        load_op_rematerializable(mf, ir_fn, ins.operands[1], SCRATCH_B);  /* val (RCX) */
                        const int32_t off = static_cast<int32_t>(ins.imm);
                        mf.blocks.back().instrs.push_back(MInstr::make_binary(MOp::MOV,
                            MOperand::make_mem(MReg::RAX, off),
                            MOperand::make_reg(SCRATCH_B), {}));
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

                /* Sprint string-perf-3 bug fix (2026-06-02): IDIV y CQO
                 * tienen RAX y RDX como dst IMPLICITOS (no en emit_mi.dst).
                 * IDIV reg: RAX = quot, RDX = rem.  CQO: sign-extend RAX
                 * -> RDX.  Sin invalidar estos regs en el tracker, la
                 * siguiente `mov rax, [slot]` despues de un IDIV se
                 * considera redundante (tracker piensa que rax aun tiene
                 * el valor pre-idiv) y se elide -> el codigo emitido
                 * NO recarga el operando del siguiente DIV/MOD, y los
                 * resultados son incorrectos.
                 *
                 * Bug capturado: `bB[1] = 48 + ((i / 10) % 10)` en bench
                 * string_workout daba '4' en lugar de '0' a partir de
                 * iter 102400 porque la segunda DIV reusaba RAX = i/10
                 * en lugar de cargar `i` de nuevo. */
                if (emit_mi.op == MOp::IDIV || emit_mi.op == MOp::CQO) {
                    invalidate_reg(static_cast<uint8_t>(MReg::RAX));
                    invalidate_reg(static_cast<uint8_t>(MReg::RDX));
                }

                out.push_back(emit_mi);
            }
            block.instrs = std::move(out);
            /* Guardar exit_state para el cross-block inheritance. */
            for (int r = 0; r < 64; ++r) {
                exit_states[bi][r] = reg_has[r];
            }
        }

        /* -------------------------------------------------------------
         * Sprint string-perf-7 (2026-06-02): store-load forwarding con
         * lookahead.
         *
         * Patron observable (e.g. xorshift en bench_branch_unpredict):
         *   1. STORE [slot]<-rX       (resultado de SHL/etc al slot)
         *   2. MOV rX, anything       (clobber de rX por la siguiente op)
         *   3. MOV rY, [slot]         (LOAD del mismo slot a otro reg)
         *
         * El peephole reg_has actual NO captura esto porque al clobber
         * rX en (2), olvida que el slot tenia el valor de rX.  En (3)
         * busca un reg con ese slot, no encuentra, y emite el LOAD.
         *
         * Optimizacion: si entre (1) y (3) hay un clobber de rX pero
         * NADIE escribe al slot ni a rY entre (1) y el primer clobber
         * de rX, INSERTAMOS `mov rY, rX` justo despues de (1) y
         * ELIMINAMOS (3).  Net: store(1byte mem write) + reg-to-reg
         * mov(0.5 ciclo) vs store + load(1byte mem read).  Ahorra el
         * mem-load (~5 ciclos cuando hay cache miss; ~1 ciclo si hit).
         * ------------------------------------------------------------- */
        {
            const uint8_t RBP_REG = reg_id(MReg::RBP);
            auto is_pure_slot = [&](const MOperand &op) -> bool {
                return op.kind == MOperandKind::MEM
                    && op.reg == RBP_REG
                    && (op.width & 0xFC) == 0xFC;
            };
            auto is_store_to_slot = [&](const MInstr &mi) -> bool {
                return mi.op == MOp::MOV
                    && mi.dst.kind == MOperandKind::MEM
                    && is_pure_slot(mi.dst)
                    && mi.src1.kind == MOperandKind::REG;
            };
            auto is_load_from_slot = [&](const MInstr &mi) -> bool {
                return mi.op == MOp::MOV
                    && mi.dst.kind == MOperandKind::REG
                    && mi.src1.kind == MOperandKind::MEM
                    && is_pure_slot(mi.src1);
            };
            auto same_slot = [](const MOperand &a, const MOperand &b) -> bool {
                return a.reg == b.reg
                    && a.value == b.value
                    && (a.width & 0xFC) == (b.width & 0xFC);
            };
            /* True si la instr escribe rX (clobber/redefine). */
            auto clobbers_reg = [&](const MInstr &mi, uint8_t r) -> bool {
                if (mi.dst.kind == MOperandKind::REG && mi.dst.reg == r)
                    return true;
                /* CALL clobbea caller-saved.  Conservativo: clobea todo. */
                if (mi.op == MOp::CALL)
                    return true;
                /* IDIV/CQO clobean RAX/RDX. */
                if (mi.op == MOp::IDIV || mi.op == MOp::CQO) {
                    if (r == static_cast<uint8_t>(MReg::RAX)
                     || r == static_cast<uint8_t>(MReg::RDX))
                        return true;
                }
                return false;
            };

            const int LOOKAHEAD = 24;  /* ventana de busqueda; basta para
                                        * patrones tipicos de xorshift +
                                        * load-store inmediatos. */
            size_t fwd_applied = 0;
            for (auto &block : mf.blocks) {
                auto &instrs = block.instrs;
                /* Marcamos LOADs a eliminar via flag; al final compact. */
                std::vector<bool> kill(instrs.size(), false);
                /* Inserts pendientes: (index_after, mov_dst_reg, mov_src_reg). */
                std::vector<std::tuple<size_t, uint8_t, uint8_t>> inserts;

                for (size_t i = 0; i < instrs.size(); ++i) {
                    if (!is_store_to_slot(instrs[i])) continue;
                    const auto &store = instrs[i];
                    const uint8_t rX = store.src1.reg;
                    const MOperand slot_mem_op = store.dst;
                    /* Si rX < 64 (= GP). */
                    if (rX >= 64) continue;

                    /* Scan ventana adelante. */
                    bool rx_clobbered = false;
                    size_t clobber_idx = 0;
                    const size_t lim = std::min(instrs.size(), i + 1 + LOOKAHEAD);
                    for (size_t j = i + 1; j < lim; ++j) {
                        const auto &mj = instrs[j];
                        /* Si STORE al MISMO slot, slot reescrito -> stop. */
                        if (is_store_to_slot(mj) && same_slot(mj.dst, slot_mem_op)) break;
                        /* Si STORE a memoria arbitraria (no slot), conservativo: stop. */
                        if (mj.op == MOp::MOV && mj.dst.kind == MOperandKind::MEM
                            && !is_pure_slot(mj.dst)) break;
                        if (mj.op == MOp::CALL) break;  /* clobea regs + memoria */

                        if (!rx_clobbered && clobbers_reg(mj, rX)) {
                            rx_clobbered = true;
                            clobber_idx = j;
                        }

                        /* Buscar LOAD del MISMO slot a otro reg. */
                        if (is_load_from_slot(mj)
                         && same_slot(mj.src1, slot_mem_op)
                         && mj.dst.reg < 64
                         && mj.dst.reg != rX) {
                            const uint8_t rY = mj.dst.reg;
                            /* Verificar que rY no fue escrito entre i+1 y j-1
                             * (si lo fue, no podemos pre-cargar). */
                            bool ry_safe = true;
                            for (size_t k = i + 1; k < j; ++k) {
                                if (clobbers_reg(instrs[k], rY)) {
                                    ry_safe = false; break;
                                }
                            }
                            if (!ry_safe) continue;
                            /* Caso A: rX NO fue clobbed entre (i+1) y j.
                             * El peephole reg_has YA podria optimizar; aqui
                             * sirve como respaldo. */
                            if (!rx_clobbered) {
                                /* Reemplazar load por mov rY, rX (sin insertar). */
                                instrs[j].src1 = MOperand::make_reg(static_cast<MReg>(rX));
                                ++fwd_applied;
                                continue;
                            }
                            /* Caso B: rX fue clobbed en clobber_idx (< j).
                             * Insertar `mov rY, rX` justo despues del store
                             * (idx i+1) y eliminar el load. */
                            inserts.emplace_back(i + 1, rY, rX);
                            kill[j] = true;
                            ++fwd_applied;
                        }
                    }
                }

                if (kill.empty() && inserts.empty()) continue;

                /* Aplicar: construir nueva lista con los inserts + sin los
                 * loads marcados.  Recorremos en orden; al llegar a un index
                 * en `inserts`, insertamos `mov rY, rX` antes. */
                std::vector<MInstr> rebuilt;
                rebuilt.reserve(instrs.size() + inserts.size());
                /* Ordenar inserts por index ASC. */
                std::sort(inserts.begin(), inserts.end(),
                          [](const auto &a, const auto &b) {
                              return std::get<0>(a) < std::get<0>(b);
                          });
                size_t ins_idx = 0;
                for (size_t i = 0; i < instrs.size(); ++i) {
                    while (ins_idx < inserts.size()
                        && std::get<0>(inserts[ins_idx]) == i) {
                        const uint8_t rY = std::get<1>(inserts[ins_idx]);
                        const uint8_t rX = std::get<2>(inserts[ins_idx]);
                        rebuilt.push_back(
                            MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(static_cast<MReg>(rY)),
                                MOperand::make_reg(static_cast<MReg>(rX))));
                        ++ins_idx;
                    }
                    if (kill[i]) continue;
                    rebuilt.push_back(instrs[i]);
                }
                /* Inserts despues del ultimo index (rare). */
                while (ins_idx < inserts.size()) {
                    const uint8_t rY = std::get<1>(inserts[ins_idx]);
                    const uint8_t rX = std::get<2>(inserts[ins_idx]);
                    rebuilt.push_back(
                        MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(static_cast<MReg>(rY)),
                            MOperand::make_reg(static_cast<MReg>(rX))));
                    ++ins_idx;
                }
                block.instrs = std::move(rebuilt);
            }
            (void)fwd_applied;
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
                        /* Invalidar equiv para regX (su valor cambia) Y
                         * para regA (m0.dst). */
                        invalidate_reg(regX_id);
                        invalidate_reg(m0.dst.reg);
                        out.push_back(collapsed);
                        /* Phase D.jit-mem-model fix: refresh m0.dst tras el
                         * collapse.  El slot-based peephole anterior
                         * elimino loads subsecuentes `mov m0.dst, [slot]`
                         * confiando en que m0.dst contenia el valor del
                         * slot (cierto antes del collapse: m0 lo cargaba).
                         * Tras el collapse, m0 no se ejecuta y m0.dst
                         * queda con valor STALE.  Emit `mov m0.dst, regX`
                         * para que m0.dst tenga el NUEVO valor.  Net:
                         * 3 instrs -> 2 instrs en vez de 3 -> 1; aun un
                         * win neto, preservando correctness.
                         *
                         * Width: usar el ancho de m2.dst (= regX size),
                         * que es el ancho semantico del valor (m0.dst
                         * width puede ser 4 si el load era a r32). */
                        if (m0.dst.kind == MOperandKind::REG
                         && m0.dst.reg != regX_id
                         && m0.dst.reg < 32 && regX_id < 32) {
                            MInstr refresh;
                            refresh.op = MOp::MOV;
                            refresh.dst = MOperand::make_reg(
                                static_cast<MReg>(m0.dst.reg),
                                m2.dst.width);
                            refresh.src1 = MOperand::make_reg(
                                static_cast<MReg>(regX_id),
                                m2.dst.width);
                            refresh.source_pc = m2.source_pc;
                            out.push_back(refresh);
                            /* Tracking: m0.dst ahora equiv a regX. */
                            reg_equiv[m0.dst.reg] =
                                static_cast<int8_t>(regX_id);
                        }
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

        /* ===== Peephole: copy coalescing (Intel front-end: reducir mov chains) =====
         *
         * El template C1 enruta casi todo por un scratch (RAX/RCX/RDX): un
         * valor se carga al scratch y luego se mueve a su destino.  Tras el
         * regalloc rewrite eso deja cadenas `mov SCRATCH, X ; mov Y, SCRATCH`
         * (reg->scratch->reg).  Si SCRATCH esta MUERTO tras el segundo mov (lo
         * habitual: la siguiente op lo reescribe), colapsamos a `mov Y, X`.
         * Cada mov eliminado es 1 uop menos en el front-end (decode + rename),
         * sin tocar el regalloc.  Es el patron que el comparator de qsort
         * repetia ~5 veces.
         *
         * Seguridad (liveness local conservadora del scratch):
         *   - cur  = `mov SCRATCH, SRC`, SCRATCH in {RAX,RCX,RDX}, ancho == nxt.
         *   - nxt  = `mov DST, SCRATCH` (src1 == SCRATCH).
         *   - SCRATCH muerto tras nxt: escaneo forward; el primer toque debe
         *     ser una RE-DEFINICION pura (un MOV cuyo dst es SCRATCH y que no
         *     lee SCRATCH).  Cualquier LECTURA de SCRATCH, un dst==SCRATCH de
         *     una op RMW (lee+escribe), o un borde (CALL/JMP/JCC/RET/LABEL/
         *     SAFEPOINT/LOAD_PROC/IDIV/CQO) o fin de bloque antes del re-def
         *     -> conservador: NO colapsar.
         *   - No `mov mem, mem` (si SRC es MEM y DST es MEM, saltar). */
        {
            auto cc_is_scratch = [](uint8_t r) -> bool {
                return r == static_cast<uint8_t>(MReg::RAX)
                    || r == static_cast<uint8_t>(MReg::RCX)
                    || r == static_cast<uint8_t>(MReg::RDX);
            };
            auto cc_reads = [](const MOperand &o, uint8_t r) -> bool {
                if (o.kind == MOperandKind::REG) return o.reg == r;
                if (o.kind == MOperandKind::MEM)
                    return o.mem_base() == static_cast<MReg>(r)
                        || o.mem_index() == static_cast<MReg>(r);
                return false;
            };
            for (auto &block : mf.blocks) {
                std::vector<MInstr> out;
                out.reserve(block.instrs.size());
                const size_t n = block.instrs.size();
                for (size_t i = 0; i < n; ++i) {
                    if (i + 1 < n) {
                        const MInstr &cur = block.instrs[i];
                        const MInstr &nxt = block.instrs[i + 1];
                        /* nxt.dst, si es MEM, NO debe usar SCRATCH en su
                         * direccion (base/index): `mov [scr+off], scr` usa scr
                         * como direccion Y como valor; al eliminar el def de
                         * scr la direccion quedaria rota. */
                        const bool nxt_dst_uses_scr =
                            nxt.dst.kind == MOperandKind::MEM
                            && (nxt.dst.mem_base() == static_cast<MReg>(cur.dst.reg)
                             || nxt.dst.mem_index() == static_cast<MReg>(cur.dst.reg));
                        /* Combos INVALIDOS al destino MEM: x86 no tiene
                         * `mov mem, mem` ni `mov mem, imm64`.  Solo se permite
                         * mov [mem] desde REG o IMM32. */
                        const bool nxt_dst_mem_bad =
                            nxt.dst.kind == MOperandKind::MEM
                            && (cur.src1.kind == MOperandKind::MEM
                             || cur.src1.kind == MOperandKind::IMM64_IDX);
                        const bool shape =
                            cur.op == MOp::MOV
                            && cur.dst.kind == MOperandKind::REG
                            && cc_is_scratch(cur.dst.reg)
                            && nxt.op == MOp::MOV
                            && nxt.src1.kind == MOperandKind::REG
                            && nxt.src1.reg == cur.dst.reg
                            && cur.dst.width == nxt.src1.width
                            && !nxt_dst_uses_scr
                            && !nxt_dst_mem_bad;
                        if (shape) {
                            const uint8_t scr = cur.dst.reg;
                            bool dead = false;
                            for (size_t j = i + 2; j < n; ++j) {
                                const MInstr &L = block.instrs[j];
                                bool reads = cc_reads(L.src1, scr)
                                          || cc_reads(L.src2, scr);
                                if (L.dst.kind == MOperandKind::MEM
                                 && cc_reads(L.dst, scr)) reads = true;
                                const bool pure_write =
                                    L.op == MOp::MOV
                                    && L.dst.kind == MOperandKind::REG
                                    && L.dst.reg == scr
                                    && !reads;
                                /* dst==scr de una op no-MOV (RMW) -> lee scr. */
                                if (L.dst.kind == MOperandKind::REG
                                 && L.dst.reg == scr && !pure_write) reads = true;
                                if (reads) { dead = false; break; }
                                if (pure_write) { dead = true; break; }
                                if (L.op == MOp::CALL || L.op == MOp::CALL_ABS
                                 || L.op == MOp::JMP || L.op == MOp::JCC
                                 || L.op == MOp::RET || L.op == MOp::LABEL_DEF
                                 || L.op == MOp::SAFEPOINT || L.op == MOp::LOAD_PROC
                                 || L.op == MOp::IDIV || L.op == MOp::CQO) {
                                    dead = false; break;
                                }
                            }
                            if (dead) {
                                MInstr c;
                                c.op = MOp::MOV;
                                c.dst = nxt.dst;
                                c.dst.width = nxt.dst.width;
                                c.src1 = cur.src1;
                                c.source_pc = nxt.source_pc;
                                out.push_back(c);
                                ++i;  /* consume cur + nxt */
                                continue;
                            }
                        }
                    }
                    out.push_back(block.instrs[i]);
                }
                block.instrs = std::move(out);
            }
        }

        /* ===== callback-ABI win: elidir LOAD_PROC muerto =====
         *
         * En un callback hoja (fast mode), el prologo carga @c ProcessVM* en
         * RBX via @c LOAD_PROC (una lectura TLS gs:[disp]).  Pero si el cuerpo
         * NUNCA usa RBX -- p.ej. un comparator de qsort que solo hace loads
         * NATIVOS sobre sus args -- esa lectura es codigo muerto.
         *
         * La eliminamos escaneando el codigo FINAL (post-regalloc; el regalloc
         * nunca asigna RBX, reservado para proc) en busca de cualquier
         * referencia a RBX que NO sea el setup (push rbx / LOAD_PROC / pop rbx).
         * Si no hay ninguna, borramos el @c LOAD_PROC del prologo.  El
         * @c push rbx / @c pop rbx se MANTIENEN (preservan el alignment del
         * frame y dejan RBX intacto = valor del caller).  Ahorra una lectura
         * TLS por invocacion (~5 ciclos x millones de llamadas en qsort/
         * wndproc).  Si el cuerpo SI usa RBX (LOAD/STORE de VM-addr, ALLOCA,
         * GETPROC, safepoint en un back-edge, GETFIELD...), el scan lo detecta
         * y conserva el LOAD_PROC -> correcto. */
        if (cb_entry && !cb_save_all) {
            auto uses_rbx = [](const MOperand &o) -> bool {
                if (o.kind == MOperandKind::REG)
                    return o.reg == static_cast<uint8_t>(MReg::RBX);
                if (o.kind == MOperandKind::MEM)
                    return o.mem_base() == MReg::RBX || o.mem_index() == MReg::RBX;
                return false;
            };
            bool rbx_used = false;
            for (const auto &blk : mf.blocks) {
                for (const auto &mi : blk.instrs) {
                    if (mi.op == MOp::LOAD_PROC) continue;       /* el def que evaluamos */
                    if (mi.op == MOp::PUSH && mi.src1.kind == MOperandKind::REG
                        && mi.src1.reg == static_cast<uint8_t>(MReg::RBX)) continue;
                    if (mi.op == MOp::POP && mi.dst.kind == MOperandKind::REG
                        && mi.dst.reg == static_cast<uint8_t>(MReg::RBX)) continue;
                    if (uses_rbx(mi.dst) || uses_rbx(mi.src1) || uses_rbx(mi.src2)) {
                        rbx_used = true;
                        break;
                    }
                }
                if (rbx_used) break;
            }
            if (!rbx_used) {
                std::vector<MInstr> kept;
                auto &pb = mf.blocks[prologue].instrs;
                kept.reserve(pb.size());
                for (auto &mi : pb) {
                    if (mi.op == MOp::LOAD_PROC) continue;       /* elidir */
                    kept.push_back(mi);
                }
                pb = std::move(kept);
            }
        }

        if (out_unsupported) *out_unsupported = unsupported;
        return mf;
    }

} // namespace jit
