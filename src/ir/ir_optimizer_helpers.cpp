/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer_helpers.cpp
 * @brief Implementacion de los helpers compartidos de los pases de
 *        optimizacion (ver ir_optimizer_internal.h).
 */

#include "ir/ir_optimizer_internal.h"

namespace ir {
namespace opt_internal {

    bool is_side_effecting(IrOp op) {
        switch (op) {
            // llamadas (pueden lanzar excepciones o modificar estado)
            case IrOp::CALL:    case IrOp::CALLIND: case IrOp::CALLVIRT:
            case IrOp::CALLN:   case IrOp::TAILCALL: case IrOp::CALLM:
            case IrOp::CALLITF:
            case IrOp::CALLCLOSURE:
            // para C2 (escape analysis + case-splitting);
            // NUNCA eliminar aunque dst sea IR_NO_VALUE.  Sin efecto en codegen
            // (emitter los trata como no-op), pero deben sobrevivir DCE.
            case IrOp::MAKE_CLOSURE:
            case IrOp::MAKE_VARIANT:
            case IrOp::MATCH_VARIANT:
            // control de flujo
            case IrOp::BR:      case IrOp::BR_COND: case IrOp::RET:
            case IrOp::UNREACHABLE:
            // excepciones
            case IrOp::THROW:   case IrOp::TRYENTER: case IrOp::TRYLEAVE:
            case IrOp::LANDINGPAD:
            // async / distribucion
            case IrOp::FUTURE:
            case IrOp::AWAIT:   case IrOp::FULFILL:  case IrOp::REJECT:
            case IrOp::MSGSEND: case IrOp::MSGRECV:  case IrOp::RSPAWN:
            // monitores
            case IrOp::MONENTER: case IrOp::MONEXIT: case IrOp::MONWAIT:
            case IrOp::MONNOTI:  case IrOp::MONNOTA:
            // memoria
            case IrOp::STORE:   case IrOp::MEMCPY:   case IrOp::SETFIELD:
            // OOP con efectos
            case IrOp::NEWOBJ:  case IrOp::NEWOBJS: case IrOp::CHECKCAST: case IrOp::UNWRAP:
            case IrOp::SPECIALIZE:
            // GC_ALLOC
            case IrOp::GC_ALLOC:
            // arrays con efectos
            case IrOp::ARRAY_ALLOC: case IrOp::ARRAY_STORE: case IrOp::GCWB_IR:
            case IrOp::GCDEREF_IR:
            // raw_alloc/raw_free
            case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
            // cadenas con efectos
            case IrOp::STRMAKE:   case IrOp::STRCAT:    case IrOp::STRCONV:
            case IrOp::STRFLAT:   case IrOp::STRINTERN: case IrOp::STRRESERVE:
            case IrOp::STRFINALIZE:
            // scheduler / proceso
            case IrOp::SPAWN:   case IrOp::RESUME:   case IrOp::YIELD:
            case IrOp::SWAPCTX: case IrOp::SPAWN_ARGS: case IrOp::SPAWN_ON:
            case IrOp::HLT:     case IrOp::PANIC:
            // asignacion
            case IrOp::ALLOCA:
            // recuperados fase B
            case IrOp::MVTAKE_IR:
            case IrOp::GC_ALLOCP:
            case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
            case IrOp::GC_HANDLE_FOR_PTR:
            case IrOp::GC_DEREF_HOST:
            case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
            case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
            case IrOp::GETSTATIC: case IrOp::SETSTATIC:
            case IrOp::FINDCLASS: case IrOp::DEFCLASS:
            case IrOp::DEFFIELD:  case IrOp::DEFMETHOD:  case IrOp::ADDADVICE:
            case IrOp::FINDMETHOD: case IrOp::FINDFIELD:
            case IrOp::SETMETHDBG:
            case IrOp::CALLSUPER:  case IrOp::PROCEED:
            case IrOp::FULFILL_HLT:
            case IrOp::STRGETBYTES:
            // ops que pueden lanzar FatalError o disparar AV recovery
            case IrOp::LOAD:
            case IrOp::DIV: case IrOp::MOD:
            case IrOp::FDIV:
            // raw_asm-elim wave 3
            case IrOp::RETHROW:
            case IrOp::SHARED_STAT:
            case IrOp::READ_VM_REG:
            case IrOp::RSPAWN_RETURN:
            // raw_asm-elim wave 2
            case IrOp::SMARTPTR_FREE:
            case IrOp::REFLECT_COUNT:
            case IrOp::REFLECT_AT:
            case IrOp::MOD_LOAD:
            case IrOp::DLOPEN:
            case IrOp::DLSYM:
            case IrOp::GETPID:  case IrOp::GETARGC: case IrOp::GETARG:
            // ensamblador incrustado
            case IrOp::RAW_ASM:
                return true;
            default:
                return false;
        }
    }

    bool is_terminator(IrOp op) {
        return op == IrOp::BR   || op == IrOp::BR_COND ||
               op == IrOp::RET  || op == IrOp::UNREACHABLE ||
               op == IrOp::THROW || op == IrOp::RETHROW ||
               op == IrOp::RSPAWN_RETURN || op == IrOp::FULFILL_HLT;
    }

    bool is_pure(IrOp op) {
        return !is_side_effecting(op);
    }

    bool is_licm_hoistable_alloc(IrOp op) {
        switch (op) {
            case IrOp::STRCAT:
            case IrOp::STRINTERN:
            case IrOp::STRCONV:
            case IrOp::STRRESERVE:
                return true;
            case IrOp::STRMAKE:
                return true;
            default:
                return false;
        }
    }

    bool strmake_reads_immutable(const IrFunction &fn, IrValueId vm_addr) {
        if (vm_addr == IR_NO_VALUE || vm_addr >= fn.values.size()) return false;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.dst != vm_addr) continue;
                switch (ins.op) {
                    case IrOp::STR_LIT_ADDR:
                        return true;
                    case IrOp::ADD:
                    case IrOp::SUB: {
                        if (ins.operands.size() != 2) return false;
                        if (strmake_reads_immutable(fn, ins.operands[0])) {
                            IrValueId off = ins.operands[1];
                            if (off < fn.values.size() && fn.values[off].is_const) {
                                return true;
                            }
                        }
                        return false;
                    }
                    case IrOp::BITCAST:
                    case IrOp::MOV:
                        if (ins.operands.size() == 1) {
                            return strmake_reads_immutable(fn, ins.operands[0]);
                        }
                        return false;
                    default:
                        return false;
                }
            }
        }
        return false;
    }

    bool is_pure_allocator_name(const std::string &name) {
        if (name.size() >= 7
         && name.compare(name.size() - 7, 7, "_shared") == 0) return false;
        if (name.size() > 6 && name.compare(0, 6, "__new_") == 0) return true;
        if (name == "vrt_newobj") return true;
        if (name == "vrt_newobj_handle") return true;
        if (name == "vrt_register_alloc") return true;
        return false;
    }

} // namespace opt_internal
} // namespace ir
