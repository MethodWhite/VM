/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/selector_helpers.cpp
 * @brief Helper functions del instruction selector (extraido de selector.cpp).
 *
 * Incluye utilidades de parseo de raw_asm, slot/load/store helpers,
 * resolucion de runtime entries, y generacion de magic numbers para
 * division por constante (Hacker's Delight).
 */

#include "jit/selector.h"

#include "jit/auto_jit.h"
#include "jit/jit_regalloc.h"
#include "vesta_rt/abi.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace jit {

    /* ----- Helpers para el mini-parser de raw_asm (Phase D.3-G) ----- */

    /** @brief Replica de @c EmitCtx::sanitize del ir_emitter: convierte
     *  cualquier caracter no-alfanumerico/no-underscore en '_'.  Usado
     *  para resolver nombres de funcion en el symbol_table del .velb,
     *  que el linker emite ya sanitizados (igual que el .vel emitido). */
    std::string sanitize_label_name(const std::string &s) {
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
    int vm_reg_slot_index(const std::string &name) noexcept {
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
    int32_t vm_reg_offset(int slot) noexcept {
        if (slot == 16) return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) - 32;
        if (slot == 17) return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) - 24;
        return static_cast<int32_t>(VESTA_PROC_REGISTERS_OFFSET) + slot * 8;
    }

    std::string trim_str(const std::string &s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
        return s.substr(a, b - a);
    }

    std::vector<std::string> split_csv(const std::string &s) {
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

    bool parse_imm_int(const std::string &s, int64_t &out) {
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
    std::string parse_mem_operand(const std::string &s) {
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
    int32_t slot_offset(ir::IrValueId vid) noexcept {
        return -8 * (static_cast<int32_t>(vid) + 1);
    }

    /** @brief MOperand de un slot stack (RBP - offset). */
    MOperand slot_mem(ir::IrValueId vid) noexcept {
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
    uint64_t resolve_runtime_entry(const std::string &name,
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
    uint64_t ir_type_size_bytes(ir::IrType t) noexcept {
        switch (t) {
            case ir::IrType::I8:  case ir::IrType::U8:  case ir::IrType::BOOL: return 1;
            case ir::IrType::I16: case ir::IrType::U16: return 2;
            case ir::IrType::I32: case ir::IrType::U32: case ir::IrType::F32: return 4;
            default: return 8;
        }
    }

    /** @brief True si el tipo entero es con signo (I8/I16/I32/I64). */
    bool ir_type_is_signed_int(ir::IrType t) noexcept {
        return t == ir::IrType::I8  || t == ir::IrType::I16
            || t == ir::IrType::I32 || t == ir::IrType::I64;
    }

    /**
     * @brief Magic-number para division signed por constante de 32 bits.
     *
     * Algoritmo de Hacker's Delight (figura 10-1): dado el divisor @p d,
     * calcula el multiplicador @p M (int32) y el shift @p s tales que
     *   q = mulhs(M, n) [+ correccion] >> s [+ sign-bit]
     * equivale a @c n/d (signed) para todo n de 32 bits.  El @c mod se
     * deriva como @c n - q*d.  Solo valido para @c |d| >= 2 no potencia
     * de 2 (esos casos los maneja IDIV / un shift).
     */
    DivMagicS32 compute_magic_s32(int32_t d) noexcept {
        const uint32_t two31 = 0x80000000u;
        uint32_t ad  = (d < 0) ? static_cast<uint32_t>(-static_cast<int64_t>(d))
                               : static_cast<uint32_t>(d);
        uint32_t t   = two31 + (static_cast<uint32_t>(d) >> 31);
        uint32_t anc = t - 1 - t % ad;       // |nc| (nc = ...)
        int      p   = 31;
        uint32_t q1  = two31 / anc;
        uint32_t r1  = two31 - q1 * anc;
        uint32_t q2  = two31 / ad;
        uint32_t r2  = two31 - q2 * ad;
        uint32_t delta;
        do {
            ++p;
            q1 = 2 * q1; r1 = 2 * r1;
            if (r1 >= anc) { ++q1; r1 -= anc; }
            q2 = 2 * q2; r2 = 2 * r2;
            if (r2 >= ad) { ++q2; r2 -= ad; }
            delta = ad - r2;
        } while (q1 < delta || (q1 == delta && r1 == 0));
        DivMagicS32 m;
        m.M = static_cast<int32_t>(q2 + 1);
        if (d < 0) m.M = -m.M;
        m.s = p - 32;
        return m;
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
    bool cb_is_leaf_safe_op(ir::IrOp op) noexcept {
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
    MCond cond_for_cmp_op(ir::IrOp op) {
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

} // namespace jit
