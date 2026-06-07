/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_select.cpp
 * @brief Implementacion del selector vreg (Phase D.7, commits 4b/4c).
 *        Ver vreg_select.h y doc/REGALLOC.md.
 *
 * Commit 4b: CONST + ALU + RET (1 bloque).
 * Commit 4c: multi-bloque con control de flujo (CMP_* / BR / BR_COND) y PHI
 * (resuelto via copias en predecesores).  La compare-and-branch se FUSIONA a
 * CMP + Jcc cuando el cond de un BR_COND lo produce un CMP_* adyacente;
 * si no, se materializa el bool (CMP + MOV 0 + SETcc) y el BR_COND hace
 * TEST + Jcc.
 *
 * Limitacion 4c: las aristas criticas (un BR_COND cuyo target tiene PHIs)
 * NO se soportan todavia -> @c vreg_select devuelve false (fallback).  El
 * caso comun de loops/if-else generados por el frontend NO las produce.
 */

#include "jit/vreg_select.h"

#include "ir/ssa_ir.h"
#include "vesta_rt/abi.h"
#include "gc/raw_allocator.h"   // Phase D.7 perf: inline slab fast-path
/* arena -> windows.h (Win32) define macros que chocan con nombres del enum
 * IrOp/IrType (CONST, VOID, etc.).  Deshacerlos para no romper ir::IrOp::CONST. */
#ifdef CONST
#  undef CONST
#endif
#ifdef VOID
#  undef VOID
#endif

#include <cstdio>
#include <cstdlib>

namespace jit {

    /** @brief Diagnostico opt-in (VESTA_JIT_VREGS_DEBUG=1) de por que una
     *  funcion no es seleccionable por el path vreg. */
    static void vreg_dbg(const char *fn, const char *op) {
        static const bool on = []{
            const char *v = std::getenv("VESTA_JIT_VREGS_DEBUG");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (on) std::fprintf(stderr, "[vreg-sel] '%s' no soportada: op %s\n", fn, op);
    }

    /** @brief Diagnostico/A-B: VESTA_JIT_NO_INLINE_DEREF=1 enruta GC_DEREF_HOST
     *  al CALL @c vrt_gc_deref en vez del inline (para medir el inline vs el
     *  runtime, o aislar un posible bug del codegen inline). */
    static bool jit_no_inline_deref() {
        static const bool off = []{
            const char *v = std::getenv("VESTA_JIT_NO_INLINE_DEREF");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        return off;
    }

    /** @brief Diagnostico/A-B: VESTA_JIT_NO_INLINE_ALLOC=1 enruta RAW_ALLOC al
     *  CALL @c vrt_raw_alloc en vez de inline-ar el fast-path del slab (para
     *  medir el inline vs el runtime, o aislar un bug del codegen inline). */
    static bool jit_no_inline_alloc() {
        static const bool off = []{
            const char *v = std::getenv("VESTA_JIT_NO_INLINE_ALLOC");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        return off;
    }

    /** @brief DIV/MOD enteros en el path de vregs via el pseudo DIVMOD_V
     *  (expandido a idiv en el rewrite usando R11 scratch para el divisor).
     *  Default ON tras validar (codegen verificado en test_vreg_vm + e2e
     *  400/2 bajo JIT; el bug de IMUL+spill que lo bloqueaba se arreglo en
     *  regalloc_rewrite).  Se puede DESACTIVAR con VESTA_JIT_VREG_IDIV=0
     *  para volver al fallback de slots (A/B). */
    static bool jit_vreg_idiv() {
        static const bool on = []{
            const char *v = std::getenv("VESTA_JIT_VREG_IDIV");
            return !(v && v[0] == '0');   // default ON; solo OFF si =0
        }();
        return on;
    }

    /** @brief Inline dispatch de CALLVIRT en el path de vregs (default ON;
     *  VESTA_JIT_NO_INLINE_CALLVIRT=1 vuelve al CALL a vrt_callvirt para A/B).
     *  Carga class_ptr -> vtable -> method -> jit_code y hace un call directo
     *  (indirecto) si el metodo esta JIT-compilado y sin advices, evitando el
     *  overhead del helper runtime; fallback a vrt_callvirt en cualquier otro
     *  caso (null, abstracto, no compilado, con aspectos AOP). */
    static bool jit_no_inline_callvirt() {
        static const bool off = []{
            const char *v = std::getenv("VESTA_JIT_NO_INLINE_CALLVIRT");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        return off;
    }

    namespace {

        /** @brief Operando VREG (clase GP) para un IrValueId. */
        inline MOperand vr(ir::IrValueId v) {
            return MOperand::make_vreg(static_cast<uint32_t>(v), RegClass::GP, 8);
        }

        /** @brief Tamano en bytes de un IrType entero/puntero. */
        inline int ir_type_bytes(ir::IrType t) {
            switch (t) {
                case ir::IrType::I8: case ir::IrType::U8: case ir::IrType::BOOL: return 1;
                case ir::IrType::I16: case ir::IrType::U16: return 2;
                case ir::IrType::I32: case ir::IrType::U32: case ir::IrType::F32: return 4;
                default: return 8;  // I64/U64/F64/PTR/HANDLE
            }
        }
        /** @brief True si el tipo entero tiene signo. */
        inline bool ir_type_signed(ir::IrType t) {
            return t == ir::IrType::I8 || t == ir::IrType::I16 ||
                   t == ir::IrType::I32 || t == ir::IrType::I64;
        }
        inline bool ir_type_is_float(ir::IrType t) {
            return t == ir::IrType::F32 || t == ir::IrType::F64;
        }

        /** @brief Operando memoria [rbx + proc->registers.regs[j]] (VM_ABI).
         *  RBX = ProcessVM* durante la funcion.  j=0 retorno, j>=1 argumentos. */
        inline MOperand vm_reg_mem(int j) {
            return MOperand::make_mem(MReg::RBX,
                VESTA_PROC_REGISTERS_OFFSET + j * VESTA_REGISTER_SIZE);
        }

        /** @brief Mapea un IrOp de ALU binaria al MOp x86, o false si no aplica. */
        inline bool bin_mop(ir::IrOp op, MOp &out) {
            switch (op) {
                case ir::IrOp::ADD: out = MOp::ADD;  return true;
                case ir::IrOp::SUB: out = MOp::SUB;  return true;
                case ir::IrOp::MUL: out = MOp::IMUL; return true;
                case ir::IrOp::AND: out = MOp::AND;  return true;
                case ir::IrOp::OR:  out = MOp::OR;   return true;
                case ir::IrOp::XOR: out = MOp::XOR;  return true;
                default:            return false;
            }
        }

        /** @brief Condicion x86 para un CMP_* del IR, o false si no es CMP. */
        inline bool cmp_cond(ir::IrOp op, MCond &cc) {
            switch (op) {
                case ir::IrOp::CMP_EQ:  cc = MCond::E;  return true;
                case ir::IrOp::CMP_NE:  cc = MCond::NE; return true;
                case ir::IrOp::CMP_LT:  cc = MCond::L;  return true;  // signed
                case ir::IrOp::CMP_GT:  cc = MCond::G;  return true;
                case ir::IrOp::CMP_LE:  cc = MCond::LE; return true;
                case ir::IrOp::CMP_GE:  cc = MCond::GE; return true;
                case ir::IrOp::CMP_ULT: cc = MCond::B;  return true;  // unsigned
                case ir::IrOp::CMP_UGT: cc = MCond::A;  return true;
                case ir::IrOp::CMP_ULE: cc = MCond::BE; return true;
                case ir::IrOp::CMP_UGE: cc = MCond::AE; return true;
                default:                return false;
            }
        }

        /** @brief MInstr CMP a, b (forma 3-op: src1=a, src2=b; el rewrite la baja). */
        inline MInstr mk_cmp(ir::IrValueId a, ir::IrValueId b) {
            MInstr i; i.op = MOp::CMP; i.src1 = vr(a); i.src2 = vr(b); return i;
        }
        /** @brief MInstr TEST a, b. */
        inline MInstr mk_test(ir::IrValueId a, ir::IrValueId b) {
            MInstr i; i.op = MOp::TEST; i.src1 = vr(a); i.src2 = vr(b); return i;
        }
        /** @brief MInstr SETcc dst (variant=cc). */
        inline MInstr mk_setcc(ir::IrValueId dst, MCond cc) {
            MInstr i; i.op = MOp::SETCC; i.dst = vr(dst);
            i.variant = static_cast<uint8_t>(cc); return i;
        }

        /** @brief True si el bloque @p b tiene alguna instr PHI. */
        inline bool block_has_phi(const ir::IrBlock &b) {
            for (const auto &in : b.instrs)
                if (in.op == ir::IrOp::PHI) return true;
            return false;
        }

    } // namespace

    bool vreg_select(const ir::IrFunction &fn, MFunction &out, AbiKind abi,
                     const CallResolver &resolve_call, const VregEntries &ent,
                     const CallResolver &resolve_native) {
        out = MFunction{};
        out.name = fn.name;
        out.vreg_count = static_cast<uint32_t>(fn.values.size());
        out.ir_value_count = static_cast<uint32_t>(fn.values.size());  // OSR: limite IR/temps
        out.vreg_class.assign(fn.values.size(), RegClass::GP);
        const bool vm = (abi == AbiKind::VM);

        /* Phase D.7 commit 5f: marcar los vregs GC.  El pipeline hace el
         * check FINO (sin stackmaps todavia): rechaza la funcion solo si un
         * valor GC esta VIVO a traves de un call (su intervalo cubre un
         * call_position) -- ese seria invisible al GC en un registro.  Los
         * receptores/args de un call van a proc->registers (que el GC SI
         * escanea) y mueren antes del call, asi que no disparan el rechazo. */
        out.vreg_is_gc.assign(fn.values.size(), 0);
        for (size_t i = 0; i < fn.values.size(); ++i) {
            if (!fn.values[i].is_gc_object) continue;
            /* Codifica StackmapGcKind+1: HOSTPTR si es host_ptr a objeto GC,
             * HANDLE en otro caso.  (STRING se trata como HOSTPTR: el scan
             * usa el mismo host_ptr al payload.) */
            const StackmapGcKind k = fn.values[i].is_host_ptr
                ? StackmapGcKind::HOSTPTR : StackmapGcKind::HANDLE;
            out.vreg_is_gc[i] = static_cast<uint8_t>(static_cast<uint8_t>(k) + 1u);
        }

        const size_t NB = fn.blocks.size();
        if (NB == 0) return false;

        /* Un label por bloque (MBlock index == IR block id). */
        std::vector<MLabelId> blbl(NB);
        for (size_t b = 0; b < NB; ++b) blbl[b] = out.new_label();

        /* Valores const conocidos (para shifts por cantidad inmediata).  El
         * IR define la cantidad de un shift con un CONST previo; lo capturamos
         * para emitir SHL/SHR/SAR dst, imm en vez de via CL. */
        std::vector<uint8_t>  v_is_const(fn.values.size(), 0);
        std::vector<int64_t>  v_const(fn.values.size(), 0);

        /* Vids que son ALLOCA host-stack (viven en el frame, liberados por el
         * epilogue).  Un RAW_FREE sobre uno de estos es NO-OP (no se llama a
         * vrt_raw_free, que crashearia sobre un ptr de host stack). */
        std::vector<uint8_t>  v_is_host_alloca(fn.values.size(), 0);
        for (const auto &blk : fn.blocks)
            for (const auto &ins2 : blk.instrs)
                if (ins2.op == ir::IrOp::ALLOCA && ins2.host_alloca
                 && ins2.dst != ir::IR_NO_VALUE
                 && ins2.dst < v_is_host_alloca.size())
                    v_is_host_alloca[ins2.dst] = 1u;

        /* Crea un vreg temporal nuevo (GP, no-GC) para secuencias inline
         * (p.ej. el inline de vmath_abs). */
        auto new_tmp = [&]() -> ir::IrValueId {
            const ir::IrValueId id = out.vreg_count++;
            out.vreg_class.push_back(RegClass::GP);
            out.vreg_is_gc.push_back(0);
            return static_cast<ir::IrValueId>(id);
        };

        for (size_t b = 0; b < NB; ++b) {
            const ir::IrBlock &ib = fn.blocks[b];
            MBlock mb;
            mb.label_id = blbl[b];
            auto &O = mb.instrs;

            /* VM_ABI: al entrar, cargar cada parametro desde
             * proc->registers.regs[i+1] (regs[0] reservado al retorno). */
            if (vm && b == 0) {
                for (size_t i = 0; i < fn.params.size(); ++i)
                    O.push_back(MInstr::make_unary(MOp::MOV, vr(fn.params[i]),
                        vm_reg_mem(static_cast<int>(i) + 1)));
            }

            /* Emite las copias de PHI para una arista (b -> target): por cada
             * PHI del target, MOV phi_dst <- (arg cuyo pred == b). */
            auto emit_phi_copies = [&](ir::IrBlockId target) {
                const ir::IrBlock &tb = fn.blocks[target];
                for (const ir::IrInstr &p : tb.instrs) {
                    if (p.op != ir::IrOp::PHI) continue;
                    for (const ir::IrPhiArg &a : p.phi_args) {
                        if (a.block == static_cast<ir::IrBlockId>(b)) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(p.dst), vr(a.value)));
                            break;
                        }
                    }
                }
            };

            /* CMP diferido para fusionar compare-and-branch. */
            bool      has_pend = false;
            MCond     pend_cc  = MCond::E;
            ir::IrValueId pend_dst = ir::IR_NO_VALUE, pend_a = 0, pend_b = 0;
            auto flush_pending = [&]() {
                if (!has_pend) return;
                O.push_back(mk_cmp(pend_a, pend_b));
                O.push_back(MInstr::make_unary(MOp::MOV, vr(pend_dst),
                                               MOperand::make_imm32(0)));
                O.push_back(mk_setcc(pend_dst, pend_cc));
                has_pend = false;
            };

            for (const ir::IrInstr &in : ib.instrs) {
                MOp  mop;
                MCond cc;
                if (in.op == ir::IrOp::PHI) continue;  // resuelto via copias

                if (cmp_cond(in.op, cc)) {
                    flush_pending();
                    if (in.operands.size() != 2) return false;
                    /* Diferir: quiza se fusione con el BR_COND siguiente. */
                    has_pend = true; pend_cc = cc; pend_dst = in.dst;
                    pend_a = in.operands[0]; pend_b = in.operands[1];
                    continue;
                }

                switch (in.op) {
                    case ir::IrOp::NOP: break;

                    case ir::IrOp::CONST: {
                        flush_pending();
                        if (in.dst < v_is_const.size()) {  // recordar para shifts
                            v_is_const[in.dst] = 1;
                            v_const[in.dst] = static_cast<int64_t>(in.imm);
                        }
                        const int64_t s = static_cast<int64_t>(in.imm);
                        if (s >= INT32_MIN && s <= INT32_MAX) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_imm32(static_cast<int32_t>(s))));
                        } else {
                            const uint32_t idx = out.intern_imm64(in.imm);
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_imm64_idx(idx)));
                        }
                        break;
                    }

                    case ir::IrOp::ADD: case ir::IrOp::SUB: case ir::IrOp::MUL:
                    case ir::IrOp::AND: case ir::IrOp::OR:  case ir::IrOp::XOR: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;
                        (void)bin_mop(in.op, mop);
                        O.push_back(MInstr::make_binary(mop, vr(in.dst),
                            vr(in.operands[0]), vr(in.operands[1])));
                        break;
                    }
                    /* DIV/MOD enteros (signed) via pseudo DIVMOD_V (opt-in
                     * VESTA_JIT_VREG_IDIV mientras se valida).  El rewrite lo
                     * expande a idiv usando R11 para el divisor (sin aliasing).
                     * variant: 0 = cociente (DIV), 1 = resto (MOD). */
                    case ir::IrOp::DIV:
                    case ir::IrOp::MOD: {
                        flush_pending();
                        if (!jit_vreg_idiv()) {
                            vreg_dbg(fn.name.c_str(),
                                in.op == ir::IrOp::DIV ? "div" : "mod");
                            return false;   // gated off -> slots
                        }
                        if (in.operands.size() != 2
                         || in.dst == ir::IR_NO_VALUE) return false;
                        MInstr dm{};
                        dm.op = MOp::DIVMOD_V;
                        dm.dst  = vr(in.dst);
                        dm.src1 = vr(in.operands[0]);   // dividendo
                        dm.src2 = vr(in.operands[1]);   // divisor
                        dm.variant = (in.op == ir::IrOp::MOD) ? 1u : 0u;
                        O.push_back(dm);
                        break;
                    }
                    case ir::IrOp::NEG: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::NEG, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }
                    case ir::IrOp::NOT: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::NOT, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }

                    /* SHL/SHR/SAR con cantidad INMEDIATA (el caso comun: el IR
                     * define la cuenta con un CONST).  dst = src << imm.  Cuenta
                     * variable (en registro) -> fallback (necesita CL). */
                    case ir::IrOp::SHL: case ir::IrOp::SHR: case ir::IrOp::SAR: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;
                        const ir::IrValueId amt_v = in.operands[1];
                        if (amt_v >= v_is_const.size() || !v_is_const[amt_v]) {
                            vreg_dbg(fn.name.c_str(), "shift-var"); return false;
                        }
                        const int32_t amt = static_cast<int32_t>(v_const[amt_v] & 63);
                        const MOp mop = (in.op == ir::IrOp::SHL) ? MOp::SHL
                                      : (in.op == ir::IrOp::SHR) ? MOp::SHR : MOp::SAR;
                        O.push_back(MInstr::make_binary(mop, vr(in.dst),
                            vr(in.operands[0]), MOperand::make_imm32(amt)));
                        break;
                    }

                    /* Math intrinsics como IrOps dedicados (el frontend baja
                     * imin/imax/ilog2/clz/... a estos, NO a CALLN vmath_*).
                     * Inline directo (principio "JIT inline > runtime"); misma
                     * secuencia que el slot selector. */
                    case ir::IrOp::IABS: {  /* |a| = (a^(a>>63)) - (a>>63) */
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                            return false;
                        const ir::IrValueId t = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t),
                            vr(in.operands[0])));
                        O.push_back(MInstr::make_binary(MOp::SAR, vr(t), vr(t),
                            MOperand::make_imm32(63)));
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            vr(in.operands[0])));
                        O.push_back(MInstr::make_binary(MOp::XOR, vr(in.dst),
                            vr(in.dst), vr(t)));
                        O.push_back(MInstr::make_binary(MOp::SUB, vr(in.dst),
                            vr(in.dst), vr(t)));
                        break;
                    }
                    case ir::IrOp::IMIN: case ir::IrOp::IMAX:
                    case ir::IrOp::IMINU: case ir::IrOp::IMAXU: {
                        /* dst = a; cmp a,b; cmov<cc> dst, b.  cc: IMIN->G,
                         * IMAX->L, IMINU->A, IMAXU->B (signed vs unsigned). */
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 2)
                            return false;
                        const ir::IrValueId a = in.operands[0], b = in.operands[1];
                        const MCond cc =
                            (in.op == ir::IrOp::IMIN)  ? MCond::G :
                            (in.op == ir::IrOp::IMAX)  ? MCond::L :
                            (in.op == ir::IrOp::IMINU) ? MCond::A : MCond::B;
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst), vr(a)));
                        O.push_back(mk_cmp(a, b));
                        MInstr cm; cm.op = MOp::CMOVCC;
                        cm.variant = static_cast<uint8_t>(cc);
                        cm.dst = vr(in.dst); cm.src1 = vr(b);
                        O.push_back(cm);
                        break;
                    }
                    case ir::IrOp::ILOG2: {  /* 63 - lzcnt(a); a==0 es UB (igual que slot) */
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                            return false;
                        O.push_back(MInstr::make_unary(MOp::LZCNT, vr(in.dst),
                            vr(in.operands[0])));
                        O.push_back(MInstr::make_unary(MOp::NEG, vr(in.dst),
                            vr(in.dst)));
                        O.push_back(MInstr::make_binary(MOp::ADD, vr(in.dst),
                            vr(in.dst), MOperand::make_imm32(63)));
                        break;
                    }
                    case ir::IrOp::CLZ: case ir::IrOp::CTZ: case ir::IrOp::POPCNT: {
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                            return false;
                        const MOp mop = (in.op == ir::IrOp::CLZ)    ? MOp::LZCNT :
                                        (in.op == ir::IrOp::CTZ)    ? MOp::TZCNT :
                                                                       MOp::POPCNT;
                        O.push_back(MInstr::make_unary(mop, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }
                    case ir::IrOp::BYTESWAP: {  /* mov dst, src; bswap dst */
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 1)
                            return false;
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            vr(in.operands[0])));
                        O.push_back(MInstr::make_unary(MOp::BSWAP, vr(in.dst),
                            vr(in.dst)));
                        break;
                    }
                    case ir::IrOp::ROTL: case ir::IrOp::ROTR: {
                        /* count CONSTANTE -> rol/ror dst, imm.  Variable -> CL
                         * (no soportado en vregs) -> fallback. */
                        flush_pending();
                        if (in.dst == ir::IR_NO_VALUE || in.operands.size() != 2)
                            return false;
                        const ir::IrValueId cnt = in.operands[1];
                        if (cnt >= v_is_const.size() || !v_is_const[cnt]) {
                            vreg_dbg(fn.name.c_str(), "rot-var"); return false;
                        }
                        const int32_t amt = static_cast<int32_t>(v_const[cnt] & 63);
                        const MOp rop = (in.op == ir::IrOp::ROTL) ? MOp::ROL : MOp::ROR;
                        O.push_back(MInstr::make_binary(rop, vr(in.dst),
                            vr(in.operands[0]), MOperand::make_imm32(amt)));
                        break;
                    }

                    /* Conversiones enteras: BITCAST (mismo ancho -> MOV),
                     * SEXT/ZEXT/CAST (extension via MOVSX/MOVZX).  Las
                     * truncaciones y los floats caen a fallback por ahora. */
                    case ir::IrOp::BITCAST: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            vr(in.operands[0])));
                        break;
                    }
                    case ir::IrOp::ZEXT: case ir::IrOp::SEXT: case ir::IrOp::CAST:
                    case ir::IrOp::TRUNC: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        const ir::IrType st = fn.values[in.operands[0]].type;
                        const ir::IrType dt = in.type;
                        if (ir_type_is_float(st) || ir_type_is_float(dt)) {
                            vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                            return false;
                        }
                        const int sb = ir_type_bytes(st), db = ir_type_bytes(dt);
                        /* MOV de ancho w (w<8 zero-extiende los bits altos). */
                        auto mov_w = [&](int w) {
                            MOperand d = vr(in.dst);            d.width = static_cast<uint8_t>(w);
                            MOperand s = vr(in.operands[0]);    s.width = static_cast<uint8_t>(w);
                            O.push_back(MInstr::make_unary(MOp::MOV, d, s));
                        };
                        /* MOVSX/MOVZX dst64 <- src<srcw>. */
                        auto ext = [&](MOp mop, int srcw) {
                            const MOperand s = MOperand::make_vreg(
                                static_cast<uint32_t>(in.operands[0]),
                                RegClass::GP, static_cast<uint8_t>(srcw));
                            O.push_back(MInstr::make_unary(mop, vr(in.dst), s));
                        };
                        if (db == sb) {
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vr(in.operands[0])));  // copia de bits
                        } else if (db > sb) {
                            /* EXTENSION (zext/sext). */
                            const bool sign = (in.op == ir::IrOp::SEXT) ||
                                (in.op == ir::IrOp::CAST && ir_type_signed(st));
                            if (sb == 1 || sb == 2) ext(sign ? MOp::MOVSX : MOp::MOVZX, sb);
                            else if (sign)          ext(MOp::MOVSX, 4);  // i32->i64 (MOVSXD)
                            else                    mov_w(4);            // u32->u64 zero-ext
                        } else {
                            /* TRUNCACION (db < sb): el signo lo da el DESTINO. */
                            const bool sign = ir_type_signed(dt);
                            if (db == 4) {
                                if (sign) ext(MOp::MOVSX, 4);  // i32: 32 bajos sign-ext
                                else      mov_w(4);            // u32: 32 bajos zero-ext
                            } else {  // db == 1 || db == 2
                                ext(sign ? MOp::MOVSX : MOp::MOVZX, db);
                            }
                        }
                        break;
                    }

                    case ir::IrOp::BR: {
                        flush_pending();
                        const ir::IrBlockId t = in.target_block;
                        emit_phi_copies(t);                       // pred 1-succ: seguro
                        O.push_back(MInstr::make_jmp(blbl[t]));
                        mb.succ_a = static_cast<MBlockId>(t);
                        break;
                    }

                    case ir::IrOp::BR_COND: {
                        if (in.operands.size() != 1) return false;
                        const ir::IrBlockId tt = in.target_block;   // true
                        const ir::IrBlockId tf = in.false_block;    // false
                        /* Arista critica (target con PHIs) no soportada en 4c. */
                        if (block_has_phi(fn.blocks[tt]) ||
                            block_has_phi(fn.blocks[tf])) return false;
                        const ir::IrValueId cond = in.operands[0];

                        if (has_pend && pend_dst == cond) {
                            /* FUSION: CMP a,b + Jcc(cc) true + JMP false. */
                            O.push_back(mk_cmp(pend_a, pend_b));
                            O.push_back(MInstr::make_jcc(pend_cc, blbl[tt]));
                            O.push_back(MInstr::make_jmp(blbl[tf]));
                            has_pend = false;
                        } else {
                            flush_pending();
                            O.push_back(mk_test(cond, cond));
                            O.push_back(MInstr::make_jcc(MCond::NE, blbl[tt]));
                            O.push_back(MInstr::make_jmp(blbl[tf]));
                        }
                        mb.succ_a = static_cast<MBlockId>(tt);
                        mb.succ_b = static_cast<MBlockId>(tf);
                        break;
                    }

                    case ir::IrOp::RET: {
                        flush_pending();
                        if (!in.operands.empty()) {
                            /* VM_ABI: escribir el resultado en
                             * proc->registers.regs[0] ([rbx+off]); host leaf:
                             * dejarlo en RAX. */
                            const MOperand dst = vm
                                ? vm_reg_mem(0)
                                : MOperand::make_reg(MReg::RAX, 8);
                            O.push_back(MInstr::make_unary(MOp::MOV, dst,
                                vr(in.operands[0])));
                        }
                        O.push_back(MInstr::make_ret());
                        break;
                    }

                    /* ALLOCA host (auto-promote, no escapa): reserva en el frame
                     * JIT -> dst = host_ptr.  Las allocas que van al VM stack
                     * (host_alloca=false) caen a fallback. */
                    case ir::IrOp::ALLOCA: {
                        flush_pending();
                        if (!in.host_alloca) {
                            vreg_dbg(fn.name.c_str(), "alloca-vm"); return false;
                        }
                        const uint64_t size = in.imm;
                        if (size == 0 || size > 65536) {  // sanity (frame chico)
                            vreg_dbg(fn.name.c_str(), "alloca-size"); return false;
                        }
                        O.push_back(MInstr::make_alloca(vr(in.dst),
                            static_cast<uint32_t>(size)));
                        break;
                    }

                    /* LOAD/STORE sobre memoria HOST (host_ptr): field access de
                     * objetos GC, malloc, etc.  La direccion ya viene calculada
                     * en un vreg (el IR emite add.ptr this+offset).  vm_mem
                     * (is_host_ptr=false) cae a fallback (necesita traduccion). */
                    case ir::IrOp::LOAD: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        if (ir_type_is_float(in.type)) {
                            vreg_dbg(fn.name.c_str(), "load-float"); return false;
                        }
                        if (!fn.values[in.operands[0]].is_host_ptr) {
                            vreg_dbg(fn.name.c_str(), "load-vm"); return false;
                        }
                        const int w = ir_type_bytes(in.type);
                        /* u32 (4 bytes unsigned) necesita mov de 32 bits zero-ext;
                         * no soportado aun -> fallback. */
                        if (w == 4 && !ir_type_signed(in.type)) {
                            vreg_dbg(fn.name.c_str(), "load-u32"); return false;
                        }
                        O.push_back(MInstr::make_load(vr(in.dst),
                            vr(in.operands[0]), static_cast<uint8_t>(w),
                            ir_type_signed(in.type)));
                        break;
                    }
                    case ir::IrOp::STORE: {
                        flush_pending();
                        if (in.operands.size() != 2) return false;  // [0]=val [1]=ptr
                        if (ir_type_is_float(in.type)) {
                            vreg_dbg(fn.name.c_str(), "store-float"); return false;
                        }
                        if (!fn.values[in.operands[1]].is_host_ptr) {
                            vreg_dbg(fn.name.c_str(), "store-vm"); return false;
                        }
                        const int w = ir_type_bytes(in.type);
                        O.push_back(MInstr::make_store(vr(in.operands[1]),
                            vr(in.operands[0]), static_cast<uint8_t>(w)));
                        break;
                    }

                    /* GC_DEREF_HOST: dst = deref(handle).  INLINE del lookup
                     * (principio "JIT inline > runtime"): replica
                     * @c GcHeap::deref leyendo la HandleTable directamente, en
                     * vez de un CALL a @c vrt_gc_deref (~6x: 30ns -> 5ns).
                     * Es el op de field-access GC mas caliente.
                     *
                     * Semantica replicada (ver gc_heap.cpp::deref):
                     *   if (h & SHARED_HANDLE_BIT) return shared_lookup(h);  // bit31
                     *   if (h >= count_ || !data_[h].live) return nullptr;
                     *   return data_[h].addr + sizeof(GcHeader);             // +8
                     * con HandleEntry{ addr@0, live@8, stride 16 } (static_assert
                     * en gc_heap.h) y HandleTable{ data_@0, count_@8 }.
                     *
                     * SEGURIDAD GC: dst es un host_ptr GC.  Si su intervalo
                     * cruza el CALL del path shared, el commit 6 lo spillea a
                     * slot + emite stackmap.  Por eso lo INICIALIZAMOS a 0
                     * (null) al principio: si el GC corre durante ese CALL, el
                     * slot contiene null (root ignorado), nunca basura. */
                    case ir::IrOp::GC_DEREF_HOST: {
                        flush_pending();
                        if (!vm || ent.gc_deref == 0) {
                            vreg_dbg(fn.name.c_str(), "gc_deref(no-vm/no-addr)");
                            return false;
                        }
                        if (in.operands.size() != 1) return false;
                        if (in.dst == ir::IR_NO_VALUE) break;  // lookup sin uso: no-op

                        /* A-B / diagnostico: enrutar al CALL vrt_gc_deref. */
                        if (jit_no_inline_deref()) {
#if defined(_WIN32)
                            const MReg ca0 = MReg::RCX, ca1 = MReg::RDX;
#else
                            const MReg ca0 = MReg::RDI, ca1 = MReg::RSI;
#endif
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ca1, 8), vr(in.operands[0])));
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(ca0, 8),
                                MOperand::make_reg(MReg::RBX, 8)));
                            O.push_back(MInstr::make_call_abs(
                                out.intern_imm64(ent.gc_deref)));
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 8)));
                            break;
                        }

                        const ir::IrValueId h    = in.operands[0];
                        const MLabelId      Lsh   = out.new_label();  // path shared
                        const MLabelId      Ldone = out.new_label();  // salida comun

                        /* ORDEN: path local (comun) como FALLTHROUGH para que el
                         * hot path no pague un branch tomado; el path shared
                         * (raro) al final.  dst se INICIALIZA a 0 al principio:
                         * (a) es el resultado de los early-exits (fuera de rango
                         * / !live); (b) como su intervalo cruza el CALL del path
                         * shared, el commit 6 lo spillea a slot + stackmap -- con
                         * dst=0 ese slot es null (GC root ignorado), nunca basura.
                         * El spill (2 ops L1 por deref) resulto mas barato que el
                         * branch tomado del orden inverso en workloads memory-bound. */

                        /* dst = 0 (null). */
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            MOperand::make_imm32(0)));

                        /* h32 = handle normalizado a 32 bits (zero-extend).  El
                         * handle es u32; un productor podria dejar bits altos
                         * sucios en el reg de 64 -> shl 32 / shr 32 los limpia. */
                        const ir::IrValueId h32 = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(h32), vr(h)));
                        O.push_back(MInstr::make_binary(MOp::SHL, vr(h32), vr(h32),
                            MOperand::make_imm32(32)));
                        O.push_back(MInstr::make_binary(MOp::SHR, vr(h32), vr(h32),
                            MOperand::make_imm32(32)));

                        /* if (h & SHARED_HANDLE_BIT) goto Lsh.  bit31 = h32 >> 31. */
                        const ir::IrValueId t_sh = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_sh), vr(h32)));
                        O.push_back(MInstr::make_binary(MOp::SHR, vr(t_sh), vr(t_sh),
                            MOperand::make_imm32(31)));
                        O.push_back(mk_test(t_sh, t_sh));
                        O.push_back(MInstr::make_jcc(MCond::NE, Lsh));

                        /* --- path local (fallthrough) --- */
                        /* base = proc->jit_handle_table  (HandleTable*). */
                        const ir::IrValueId t_base = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_base),
                            MOperand::make_mem(MReg::RBX,
                                VESTA_PROC_JIT_HANDLE_TABLE_OFFSET)));
                        /* data_ = [base + 0]  (HandleEntry*). */
                        const ir::IrValueId t_data = new_tmp();
                        O.push_back(MInstr::make_load(vr(t_data), vr(t_base), 8, false));
                        /* count_ = low32 de [base + 8].  Cargamos 8 bytes
                         * ((cap_<<32)|count_) y aislamos los 32 bajos con
                         * shl 32 / shr 32 (un LOAD width-4 unsigned seria un
                         * MOVZX r/m32 invalido). */
                        const ir::IrValueId t_b8 = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_b8), vr(t_base)));
                        O.push_back(MInstr::make_binary(MOp::ADD, vr(t_b8), vr(t_b8),
                            MOperand::make_imm32(8)));
                        const ir::IrValueId t_cnt = new_tmp();
                        O.push_back(MInstr::make_load(vr(t_cnt), vr(t_b8), 8, false));
                        O.push_back(MInstr::make_binary(MOp::SHL, vr(t_cnt), vr(t_cnt),
                            MOperand::make_imm32(32)));
                        O.push_back(MInstr::make_binary(MOp::SHR, vr(t_cnt), vr(t_cnt),
                            MOperand::make_imm32(32)));
                        /* if (h32 >= count_) goto Ldone  (dst sigue 0).  Unsigned. */
                        O.push_back(mk_cmp(h32, t_cnt));
                        O.push_back(MInstr::make_jcc(MCond::AE, Ldone));
                        /* entry = data_ + h32 * 16. */
                        const ir::IrValueId t_idx = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_idx), vr(h32)));
                        O.push_back(MInstr::make_binary(MOp::SHL, vr(t_idx), vr(t_idx),
                            MOperand::make_imm32(4)));
                        const ir::IrValueId t_entry = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_entry), vr(t_data)));
                        O.push_back(MInstr::make_binary(MOp::ADD, vr(t_entry), vr(t_entry),
                            vr(t_idx)));
                        /* live = [entry + 8]  (byte).  if (!live) goto Ldone. */
                        const ir::IrValueId t_la = new_tmp();
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(t_la), vr(t_entry)));
                        O.push_back(MInstr::make_binary(MOp::ADD, vr(t_la), vr(t_la),
                            MOperand::make_imm32(8)));
                        const ir::IrValueId t_live = new_tmp();
                        O.push_back(MInstr::make_load(vr(t_live), vr(t_la), 1, false));
                        O.push_back(mk_test(t_live, t_live));
                        O.push_back(MInstr::make_jcc(MCond::E, Ldone));
                        /* dst = [entry + 0] (addr) + sizeof(GcHeader)=8. */
                        O.push_back(MInstr::make_load(vr(in.dst), vr(t_entry), 8, false));
                        O.push_back(MInstr::make_binary(MOp::ADD, vr(in.dst), vr(in.dst),
                            MOperand::make_imm32(8)));
                        O.push_back(MInstr::make_jmp(Ldone));

                        /* --- path shared (raro): fallback CALL vrt_gc_deref(proc, h) --- */
                        O.push_back(MInstr::make_label_def(Lsh));
#if defined(_WIN32)
                        const MReg sa0 = MReg::RCX, sa1 = MReg::RDX;
#else
                        const MReg sa0 = MReg::RDI, sa1 = MReg::RSI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(sa1, 8), vr(h)));    // arg1 = handle
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(sa0, 8),
                            MOperand::make_reg(MReg::RBX, 8)));     // arg0 = proc
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.gc_deref)));
                        O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                            MOperand::make_reg(MReg::RAX, 8)));     // dst = resultado
                        /* cae a Ldone */

                        O.push_back(MInstr::make_label_def(Ldone));
                        break;
                    }

                    /* GC_HANDLE_FOR_PTR: dst = vrt_gc_handle_for_ptr(proc, ptr).
                     * Lookup en ptr_to_handle_ (unordered_map): mas dificil de
                     * inline-ar que deref -> sigue via CALL.  Convencion host:
                     * proc=arg0, valor=arg1, resultado en RAX. */
                    case ir::IrOp::GC_HANDLE_FOR_PTR:
                    case ir::IrOp::GC_ALLOC:
                    case ir::IrOp::GC_ALLOCP:
                    case ir::IrOp::RAW_ALLOC: {
                        flush_pending();
                        /* === Inline slab fast-path (Phase D.7 perf, 2026-06-06) ===
                         * Para RAW_ALLOC con size CONSTANTE que cae en una size
                         * class pequena del slab, inline-amos el pop del free
                         * list (sin CALL al runtime), con fallback CALL @c alloc
                         * cuando el free list de esa clase esta vacio (grow) o
                         * el slab esta deshabilitado (VESTA_NO_SLAB -> free list
                         * siempre null -> siempre slow path, correcto).
                         * Replica EXACTA de RawAllocator::alloc (slab branch):
                         *   node = slab_free_list_[cls];
                         *   if (!node) goto slow;            // grow
                         *   slab_free_list_[cls] = node->next;
                         *   memset(node, 0, SLAB_SIZES[cls]);
                         *   total_bytes_ += SLAB_SIZES[cls];
                         *   return node;
                         * (alloc_count/peak son introspeccion -> se omiten.) */
                        if (in.op == ir::IrOp::RAW_ALLOC && vm
                         && ent.raw_alloc != 0 && in.dst != ir::IR_NO_VALUE
                         && in.operands.size() == 1 && !jit_no_inline_alloc()) {
                            const ir::IrValueId szv = in.operands[0];
                            if (szv < v_is_const.size() && v_is_const[szv]) {
                                const uint64_t size =
                                    static_cast<uint64_t>(v_const[szv]);
                                const size_t cls =
                                    gc::RawAllocator::jit_slab_class_for(size);
                                const uint64_t slab_size =
                                    (cls == SIZE_MAX) ? 0
                                    : gc::RawAllocator::jit_slab_size(cls);
                                /* Solo clases pequenas: zero-init unrolled barato
                                 * (<=64B = <=8 stores).  Mayores -> CALL (16+
                                 * stores no compensan el ahorro del CALL; medido
                                 * 0% en mem_malloc_free, cuyo cuello es el free).
                                 * mem_struct (Punto 8B -> clase 16) gana 6.14x. */
                                if (cls != SIZE_MAX && slab_size <= 64
                                 && slab_size >= 8) {
                                    const int32_t ra =
                                        vesta_rt::kProcRawAllocOffset;
                                    const int32_t fl_off = ra
                                      + static_cast<int32_t>(
                                          gc::RawAllocator::jit_slab_free_list_offset())
                                      + static_cast<int32_t>(cls * 8);
                                    const int32_t tb_off = ra
                                      + static_cast<int32_t>(
                                          gc::RawAllocator::jit_total_bytes_offset());
                                    const MLabelId Lslow  = out.new_label();
                                    const MLabelId Ldone2 = out.new_label();
                                    /* fl = [RBX + fl_off]  (slab_free_list_[cls]) */
                                    const ir::IrValueId t_fl = new_tmp();
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(t_fl),
                                        MOperand::make_mem(MReg::RBX, fl_off)));
                                    /* if (fl == 0) goto slow (free list vacio). */
                                    O.push_back(mk_test(t_fl, t_fl));
                                    O.push_back(MInstr::make_jcc(MCond::E, Lslow));
                                    /* next = [fl] ; slab_free_list_[cls] = next */
                                    const ir::IrValueId t_next = new_tmp();
                                    O.push_back(MInstr::make_load(vr(t_next),
                                        vr(t_fl), 8, false));
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, fl_off),
                                        vr(t_next)));
                                    /* zero-init slot: [fl + k] = 0, k=0..slab_size */
                                    const ir::IrValueId t_zero = new_tmp();
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(t_zero), MOperand::make_imm32(0)));
                                    for (uint64_t k = 0; k < slab_size; k += 8) {
                                        if (k == 0) {
                                            O.push_back(MInstr::make_store(
                                                vr(t_fl), vr(t_zero), 8));
                                        } else {
                                            const ir::IrValueId t_a = new_tmp();
                                            O.push_back(MInstr::make_unary(MOp::MOV,
                                                vr(t_a), vr(t_fl)));
                                            O.push_back(MInstr::make_binary(MOp::ADD,
                                                vr(t_a), vr(t_a),
                                                MOperand::make_imm32(
                                                    static_cast<int32_t>(k))));
                                            O.push_back(MInstr::make_store(
                                                vr(t_a), vr(t_zero), 8));
                                        }
                                    }
                                    /* total_bytes_ += slab_size */
                                    const ir::IrValueId t_tb = new_tmp();
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(t_tb),
                                        MOperand::make_mem(MReg::RBX, tb_off)));
                                    O.push_back(MInstr::make_binary(MOp::ADD,
                                        vr(t_tb), vr(t_tb),
                                        MOperand::make_imm32(
                                            static_cast<int32_t>(slab_size))));
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_mem(MReg::RBX, tb_off),
                                        vr(t_tb)));
                                    /* dst = fl ; jmp done */
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(in.dst), vr(t_fl)));
                                    O.push_back(MInstr::make_jmp(Ldone2));
                                    /* slow: dst = vrt_raw_alloc(proc, size) */
                                    O.push_back(MInstr::make_label_def(Lslow));
#if defined(_WIN32)
                                    const MReg za0 = MReg::RCX, za1 = MReg::RDX;
#else
                                    const MReg za0 = MReg::RDI, za1 = MReg::RSI;
#endif
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(za1, 8), vr(szv)));
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        MOperand::make_reg(za0, 8),
                                        MOperand::make_reg(MReg::RBX, 8)));
                                    O.push_back(MInstr::make_call_abs(
                                        out.intern_imm64(ent.raw_alloc)));
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(in.dst),
                                        MOperand::make_reg(MReg::RAX, 8)));
                                    O.push_back(MInstr::make_label_def(Ldone2));
                                    break;
                                }
                            }
                        }
                        /* GC_ALLOC y GC_ALLOCP bajan ambos a `gcallocp` en el
                         * ir_emitter -> host_ptr al payload (NO un handle).  Por
                         * eso usan gc_alloc_payload, no gc_alloc (que devuelve
                         * handle).  El slot selector mapea GC_ALLOC->gc_alloc
                         * (handle) por error -> store al handle como host_ptr =
                         * crash (era el bug de 64_curry/102/167).  gc_alloc
                         * DISPARA GC (safepoint); los GC roots vivos a traves
                         * del call los spillea el commit 6 (call_position). */
                        const uint64_t addr =
                            (in.op == ir::IrOp::GC_HANDLE_FOR_PTR) ? ent.gc_handle :
                            (in.op == ir::IrOp::RAW_ALLOC)         ? ent.raw_alloc :
                                                                     ent.gc_allocp;
                        if (!vm || addr == 0) {
                            vreg_dbg(fn.name.c_str(), "gc_runtime"); return false;
                        }
                        if (in.operands.size() != 1) return false;
#if defined(_WIN32)
                        const MReg ga0 = MReg::RCX, ga1 = MReg::RDX;
#else
                        const MReg ga0 = MReg::RDI, ga1 = MReg::RSI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(ga1, 8), vr(in.operands[0])));  // valor primero
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(ga0, 8), MOperand::make_reg(MReg::RBX, 8)));
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 8)));
                        break;
                    }

                    /* RAW_FREE(ptr) -> void.  free(ptr) del runtime.
                     *  - ptr de ALLOCA host-stack  -> NO-OP (lo libera el
                     *    epilogue; vrt_raw_free sobre un host-stack ptr crashea).
                     *    Es el caso mas inline posible (cero runtime).
                     *  - ptr de heap (RAW_ALLOC)   -> CALL vrt_raw_free.  El
                     *    free del slab allocator (free lists + size class) es
                     *    demasiado complejo para inline-arlo limpiamente (igual
                     *    que gc_handle_for_ptr); se queda como CALL. */
                    case ir::IrOp::RAW_FREE: {
                        flush_pending();
                        if (in.operands.size() != 1) return false;
                        const ir::IrValueId p = in.operands[0];
                        if (p < v_is_host_alloca.size() && v_is_host_alloca[p])
                            break;  // host-stack: no-op
                        if (!vm || ent.raw_free == 0) {
                            vreg_dbg(fn.name.c_str(), "raw_free(no-vm/no-addr)");
                            return false;
                        }
#if defined(_WIN32)
                        const MReg fa0 = MReg::RCX, fa1 = MReg::RDX;
#else
                        const MReg fa0 = MReg::RDI, fa1 = MReg::RSI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(fa1, 8), vr(p)));            // arg1 = ptr
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(fa0, 8),
                            MOperand::make_reg(MReg::RBX, 8)));             // arg0 = proc
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(ent.raw_free)));
                        break;
                    }

                    /* CALL intra-modulo a otra funcion Vex (VM_ABI): los args
                     * van a proc->registers.regs[1..N] (NO a arg_regs host),
                     * proc en RCX/RDI, resultado en regs[0]. */
                    case ir::IrOp::CALL: {
                        flush_pending();
                        if (!vm || !resolve_call) {
                            vreg_dbg(fn.name.c_str(), "call(no-vm/no-resolver)");
                            return false;
                        }
                        const uint64_t addr = resolve_call(in.func_name);
                        if (addr == 0) {
                            vreg_dbg(fn.name.c_str(), "call-unresolved");
                            return false;
                        }
                        /* 1. Stores de args a proc->registers.regs[i+1]. */
                        for (size_t i = 0; i < in.operands.size(); ++i)
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                vm_reg_mem(static_cast<int>(i) + 1),
                                vr(in.operands[i])));
                        /* 2. proc (=RBX) al primer arg host del callee. */
#if defined(_WIN32)
                        const MReg proc_reg = MReg::RCX;
#else
                        const MReg proc_reg = MReg::RDI;
#endif
                        O.push_back(MInstr::make_unary(MOp::MOV,
                            MOperand::make_reg(proc_reg, 8),
                            MOperand::make_reg(MReg::RBX, 8)));
                        /* 3. CALL a la direccion resuelta. */
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(addr)));
                        /* 4. Resultado desde regs[0]. */
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vm_reg_mem(0)));
                        break;
                    }

                    /* CALLVIRT: dispatch dinamico via vrt_callvirt(proc, obj,
                     * vtbl_idx).  Los args reales van a proc->registers; proc
                     * /obj/vtbl_idx van a arg_regs host; el resultado queda en
                     * regs[0]. */
                    case ir::IrOp::CALLVIRT: {
                        flush_pending();
                        if (!vm || ent.callvirt == 0) {
                            vreg_dbg(fn.name.c_str(), "callvirt(no-vm/no-addr)");
                            return false;
                        }
                        if (in.operands.empty()) return false;
                        const ir::IrValueId obj = in.operands[0];
                        const uint32_t vtbl_idx = static_cast<uint32_t>(in.imm);
                        /* 1. Stores de args reales a proc->registers.regs[i+1]
                         *    (operands[0]=obj=this -> regs[1], args -> regs[2..]). */
                        for (size_t i = 0; i < in.operands.size(); ++i)
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                vm_reg_mem(static_cast<int>(i) + 1),
                                vr(in.operands[i])));
                        /* 2. arg regs host (arg0=proc, arg1=obj, arg2=vtbl_idx). */
#if defined(_WIN32)
                        const MReg pr_reg = MReg::RCX, obj_reg = MReg::RDX, idx_reg = MReg::R8;
#else
                        const MReg pr_reg = MReg::RDI, obj_reg = MReg::RSI, idx_reg = MReg::RDX;
#endif
                        auto emit_callvirt_slow = [&]() {
                            /* vrt_callvirt(proc, obj, vtbl_idx).  obj PRIMERO. */
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(obj_reg, 8), vr(obj)));
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(pr_reg, 8),
                                MOperand::make_reg(MReg::RBX, 8)));
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(idx_reg, 8),
                                MOperand::make_imm32(static_cast<int32_t>(vtbl_idx))));
                            O.push_back(MInstr::make_call_abs(
                                out.intern_imm64(ent.callvirt)));
                        };
                        if (!jit_no_inline_callvirt()) {
                            /* 3a. INLINE DISPATCH: class_ptr -> vtable -> method
                             *     -> jit_code y call directo (indirecto) si el
                             *     metodo esta compilado y sin advices.  Fallback
                             *     a vrt_callvirt en cualquier otro caso. */
                            const MLabelId Lfb   = out.new_label();
                            const MLabelId Ldone = out.new_label();
                            auto load_field =
                                [&](ir::IrValueId base, int32_t off) -> ir::IrValueId {
                                const ir::IrValueId d = new_tmp();
                                if (off == 0) {
                                    O.push_back(MInstr::make_load(vr(d), vr(base),
                                                                  8, false));
                                } else {
                                    const ir::IrValueId addr = new_tmp();
                                    O.push_back(MInstr::make_unary(MOp::MOV,
                                        vr(addr), vr(base)));
                                    O.push_back(MInstr::make_binary(MOp::ADD,
                                        vr(addr), vr(addr),
                                        MOperand::make_imm32(off)));
                                    O.push_back(MInstr::make_load(vr(d), vr(addr),
                                                                  8, false));
                                }
                                return d;
                            };
                            /* cls = [obj]  (class_ptr offset 0). */
                            const ir::IrValueId cls = load_field(obj, 0);
                            O.push_back(mk_test(cls, cls));
                            O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                            /* vtbl = [cls + VTABLE_OFFSET]. */
                            const ir::IrValueId vtbl =
                                load_field(cls, VESTA_CLASSINFO_VTABLE_OFFSET);
                            O.push_back(mk_test(vtbl, vtbl));
                            O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                            /* method = [vtbl + vtbl_idx*8]. */
                            const ir::IrValueId method = load_field(vtbl,
                                static_cast<int32_t>(vtbl_idx * 8u));
                            O.push_back(mk_test(method, method));
                            O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                            /* advice = [method + ADVICE_CHAIN_OFFSET]; si != 0
                             * (tiene aspectos AOP) -> slow path. */
                            const ir::IrValueId adv = load_field(method,
                                VESTA_METHODINFO_ADVICE_CHAIN_OFFSET);
                            O.push_back(mk_test(adv, adv));
                            O.push_back(MInstr::make_jcc(MCond::NE, Lfb));
                            /* code = [method + JIT_CODE_OFFSET]; si 0 -> slow. */
                            const ir::IrValueId code = load_field(method,
                                VESTA_METHODINFO_JIT_CODE_OFFSET);
                            O.push_back(mk_test(code, code));
                            O.push_back(MInstr::make_jcc(MCond::E, Lfb));
                            /* FAST: proc en arg0; call directo a code (los args
                             * ya estan en proc->registers). */
                            O.push_back(MInstr::make_unary(MOp::MOV,
                                MOperand::make_reg(pr_reg, 8),
                                MOperand::make_reg(MReg::RBX, 8)));
                            { MInstr ic; ic.op = MOp::CALL; ic.src1 = vr(code);
                              O.push_back(ic); }
                            O.push_back(MInstr::make_jmp(Ldone));
                            /* FALLBACK. */
                            O.push_back(MInstr::make_label_def(Lfb));
                            emit_callvirt_slow();
                            O.push_back(MInstr::make_label_def(Ldone));
                        } else {
                            emit_callvirt_slow();
                        }
                        /* 4. Resultado en regs[0]. */
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vm_reg_mem(0)));
                        break;
                    }

                    /* CALLN: FFI a funcion nativa.  CALL DIRECTO a la direccion
                     * resuelta en compile-time (resolve_native) -- NO via el
                     * dispatcher runtime vrt_calln (mas lento).  Los args van a
                     * arg_regs host (convencion C) via ARG; CALL_ABS hace el
                     * parallel-move + CALL.  Resultado en RAX. */
                    case ir::IrOp::CALLN: {
                        flush_pending();
                        /* Inline de math intrinsics (principio: evitar el
                         * runtime).  vmath_abs -> sar/xor/sub (~4 instr) en vez
                         * de un CALL a vesta_math. */
                        if (in.func_name.find("vmath_abs") != std::string::npos
                         && in.dst != ir::IR_NO_VALUE && in.operands.size() == 1) {
                            const ir::IrValueId tmp = new_tmp();
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(tmp),
                                vr(in.operands[0])));
                            O.push_back(MInstr::make_binary(MOp::SAR, vr(tmp),
                                vr(tmp), MOperand::make_imm32(63)));
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                vr(in.operands[0])));
                            O.push_back(MInstr::make_binary(MOp::XOR, vr(in.dst),
                                vr(in.dst), vr(tmp)));
                            O.push_back(MInstr::make_binary(MOp::SUB, vr(in.dst),
                                vr(in.dst), vr(tmp)));
                            break;
                        }
                        /* vmath_ilog2(n) = 63 - lzcnt(n).  Paridad EXACTA con el
                         * slot selector, incluido el edge n==0 (lzcnt(0)=64 ->
                         * -1), que diverge del builtin C (0) pero mantiene
                         * slots==vregs (el frontend no genera ilog2(0)). */
                        if (in.func_name.find("vmath_ilog2") != std::string::npos
                         && in.dst != ir::IR_NO_VALUE && in.operands.size() == 1) {
                            O.push_back(MInstr::make_unary(MOp::LZCNT, vr(in.dst),
                                vr(in.operands[0])));
                            O.push_back(MInstr::make_unary(MOp::NEG, vr(in.dst),
                                vr(in.dst)));
                            O.push_back(MInstr::make_binary(MOp::ADD, vr(in.dst),
                                vr(in.dst), MOperand::make_imm32(63)));
                            break;
                        }
                        /* vmath_rotl/rotr con count CONSTANTE -> ROL/ROR dst, imm.
                         * Count variable necesita CL -> fallback al CALL. */
                        if ((in.func_name.find("vmath_rotl") != std::string::npos
                          || in.func_name.find("vmath_rotr") != std::string::npos)
                         && in.dst != ir::IR_NO_VALUE && in.operands.size() == 2) {
                            const ir::IrValueId cnt = in.operands[1];
                            if (cnt < v_is_const.size() && v_is_const[cnt]) {
                                const int32_t amt =
                                    static_cast<int32_t>(v_const[cnt] & 63);
                                const MOp rop =
                                    (in.func_name.find("vmath_rotl") != std::string::npos)
                                    ? MOp::ROL : MOp::ROR;
                                O.push_back(MInstr::make_binary(rop, vr(in.dst),
                                    vr(in.operands[0]), MOperand::make_imm32(amt)));
                                break;
                            }
                            /* count variable -> cae al CALL de abajo. */
                        }
                        /* vmath_min/max/minu/maxu -> CMP + CMOVcc (dst=a; si
                         * cc(a,b) dst=b).  Paridad EXACTA con el slot: minu->A,
                         * maxu->B, min->G (signed), max->L (signed). */
                        if ((in.func_name.find("vmath_min") != std::string::npos
                          || in.func_name.find("vmath_max") != std::string::npos)
                         && in.dst != ir::IR_NO_VALUE && in.operands.size() == 2) {
                            const ir::IrValueId a = in.operands[0];
                            const ir::IrValueId b = in.operands[1];
                            MCond cc;
                            if (in.func_name.find("vmath_minu") != std::string::npos)
                                cc = MCond::A;
                            else if (in.func_name.find("vmath_maxu") != std::string::npos)
                                cc = MCond::B;
                            else if (in.func_name.find("vmath_min") != std::string::npos)
                                cc = MCond::G;
                            else
                                cc = MCond::L;   // vmath_max
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst), vr(a)));
                            O.push_back(mk_cmp(a, b));
                            MInstr cm; cm.op = MOp::CMOVCC;
                            cm.variant = static_cast<uint8_t>(cc);
                            cm.dst = vr(in.dst); cm.src1 = vr(b);
                            O.push_back(cm);
                            break;
                        }
                        if (!resolve_native) {
                            vreg_dbg(fn.name.c_str(), "calln(no-resolver)"); return false;
                        }
                        const uint64_t fn_addr = resolve_native(in.func_name);
                        if (fn_addr == 0) {
                            vreg_dbg(fn.name.c_str(), "calln-unresolved"); return false;
                        }
                        const size_t nargs = in.operands.size();
#if defined(_WIN32)
                        const size_t nmax = 4;
#else
                        const size_t nmax = 6;
#endif
                        if (nargs > nmax) {  // stack args no soportados
                            vreg_dbg(fn.name.c_str(), "calln-args"); return false;
                        }
                        for (size_t a = 0; a < nargs; ++a)
                            O.push_back(MInstr::make_arg(static_cast<uint8_t>(a),
                                vr(in.operands[a])));
                        O.push_back(MInstr::make_call_abs(out.intern_imm64(fn_addr)));
                        if (in.dst != ir::IR_NO_VALUE)
                            O.push_back(MInstr::make_unary(MOp::MOV, vr(in.dst),
                                MOperand::make_reg(MReg::RAX, 8)));
                        break;
                    }

                    default:
                        vreg_dbg(fn.name.c_str(), ir::ir_op_name(in.op));
                        return false;  // op fuera del subset -> fallback
                }
            }
            flush_pending();  // por si el bloque termina sin terminador explicito
            out.blocks.push_back(std::move(mb));
        }
        return true;
    }

} // namespace jit
