/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer.cpp
 * @brief Implementacion de los pases de optimizacion sobre la SSA IR.
 */

#include "ir/ir_optimizer.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <functional>
#include <sstream>
#include <algorithm>

namespace ir {

// =========================================================================
//  Utilidades internas
// =========================================================================

/** @brief Devuelve true si la instruccion tiene efectos laterales visibles. */
static bool is_side_effecting(IrOp op) {
    switch (op) {
        // llamadas (pueden lanzar excepciones o modificar estado)
        case IrOp::CALL:    case IrOp::CALLIND: case IrOp::CALLVIRT:
        case IrOp::CALLN:   case IrOp::TAILCALL: case IrOp::CALLM:
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
        case IrOp::AWAIT:   case IrOp::FULFILL:  case IrOp::REJECT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:  case IrOp::RSPAWN:
        // monitores
        case IrOp::MONENTER: case IrOp::MONEXIT: case IrOp::MONWAIT:
        case IrOp::MONNOTI:  case IrOp::MONNOTA:
        // memoria
        case IrOp::STORE:   case IrOp::MEMCPY:   case IrOp::SETFIELD:
        // OOP con efectos
        case IrOp::NEWOBJ:  case IrOp::CHECKCAST: case IrOp::UNWRAP:
        case IrOp::SPECIALIZE:
        // GC_ALLOC: consume memoria del heap GC + puede disparar minor/major
        // GC + el payload puede ser referenciado posteriormente.  Tratarlo
        // como CALL evita que el DCE elimine el alloc cuando el dst es
        // temporariamente parecido a "no usado" en algun analisis local.
        case IrOp::GC_ALLOC:
        // arrays con efectos
        case IrOp::ARRAY_ALLOC: case IrOp::ARRAY_STORE: case IrOp::GCWB_IR:
        case IrOp::GCDEREF_IR:
        // raw_alloc/raw_free: cada alloc devuelve un host_ptr UNICO; no
        // se pueden deduplicar.  raw_free libera memoria (side-effect).
        case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
        // cadenas con efectos (alloc o mutacion)
        case IrOp::STRMAKE:   case IrOp::STRCAT:    case IrOp::STRCONV:
        case IrOp::STRFLAT:   case IrOp::STRINTERN: case IrOp::STRRESERVE:
        case IrOp::STRFINALIZE:
        // scheduler / proceso
        case IrOp::SPAWN:   case IrOp::RESUME:   case IrOp::YIELD:
        case IrOp::SWAPCTX: case IrOp::SPAWN_ARGS:
        // asignacion
        case IrOp::ALLOCA:
        // ensamblador incrustado (nunca eliminar; semantica opaca)
        case IrOp::RAW_ASM:
            return true;
        default:
            return false;
    }
}

/** @brief Devuelve true si la instruccion es un terminador de bloque. */
static bool is_terminator(IrOp op) {
    return op == IrOp::BR   || op == IrOp::BR_COND ||
           op == IrOp::RET  || op == IrOp::UNREACHABLE ||
           op == IrOp::THROW;
}

/** @brief Devuelve true si la instruccion es pura (apta para DCE y CSE). */
static bool is_pure(IrOp op) {
    return !is_side_effecting(op);
}

// =========================================================================
//  Pase DCE (Dead Code Elimination)
// =========================================================================

// =========================================================================
//  Pase Dead Alloc Elimination
// =========================================================================
//
// Elimina CALLs a funciones synthetic @c __new_<X> (helpers de allocacion
// emitidos por el frontend Vex para @c new ClassName()) cuyo resultado
// nunca se usa.  Estos calls solo hacen @c newobj + @c gcderef + @c mov
// internos y no tienen otros efectos secundarios observables salvo
// presion GC -- que es aceptable eliminar para casos no referenciados.
//
// Patron tipico:
//   pruebas() { Calculadora a = new Calculadora(); return 0; }
//   IR:
//     %0 = call.ptr @__new_Calculadora()
//     %1 = const.i64 0
//     %2 = trunc.i32 %1
//     ret.i32 %2
// %0 nunca se usa.  La call a @c __new_Calculadora se puede eliminar.
//
// Tambien aplica a calls a @c vrt_newobj y otros allocators puros.

/** @brief True si el nombre identifica un allocator puro (sin efectos
 *  observables salvo presion GC). */
static bool is_pure_allocator_name(const std::string &name) {
    /* Frontend Vex emite @c __new_<ClassName> para cada @c new X(). */
    if (name.size() > 6 && name.compare(0, 6, "__new_") == 0) return true;
    /* Runtime entries de alloc puros. */
    if (name == "vrt_newobj") return true;
    if (name == "vrt_newobj_handle") return true;
    if (name == "vrt_register_alloc") return true;
    return false;
}

bool ir_pass_dead_alloc_elim(IrFunction &fn) {
    /* Pasada 1: encontrar valores usados (mismo que DCE). */
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    /* Pasada 2: eliminar CALLs a allocators puros cuyo dst no se usa. */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            if (ins.op == IrOp::CALL
             && ins.dst != IR_NO_VALUE
             && !used.count(ins.dst)
             && !ins.preserve
             && is_pure_allocator_name(ins.func_name)) {
                /* CALL a allocator puro, resultado no usado -> eliminar.
                 * El frontend Vex no espera efectos secundarios visibles
                 * de @c new X() salvo el handle/host_ptr (que se descarta). */
                keep = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_simplify
// =========================================================================
//
// Aplica identidades algebraicas, plegado de constantes a traves de
// casts, y simplificacion de phis triviales.  Beneficia IGUAL al JIT
// (menos instrucciones emitidas) y a los port targets (codigo C mas
// limpio).
//
// Patrones reconocidos:
//   (A) Algebraic identities:
//       add x, 0     -> x
//       sub x, 0     -> x
//       mul x, 1     -> x
//       mul x, 0     -> 0
//       and x, 0     -> 0
//       and x, -1    -> x
//       or  x, 0     -> x
//       or  x, -1    -> -1
//       xor x, 0     -> x
//       sub x, x     -> 0
//       xor x, x     -> 0
//       shl x, 0, shr x, 0, sar x, 0 -> x
//
//   (B) Cast de constantes:
//       sext.T (const.U K)   -> const.T sign_extend(K)
//       zext.T (const.U K)   -> const.T zero_extend(K)
//       trunc.T (const.U K)  -> const.T K & mask(T)
//       bitcast.T (const.U K) -> const.T K  (mismo ancho ya)
//       cast.T (const.U K)   -> const.T K   (best-effort)
//
//   (C) Phi simplification:
//       %a = phi(b, b, b)   -> %a = mov b   (todos iguales)
//       %a = phi(a, x, y)   -> ignorar self-ref; si solo queda 1 unique -> %a = mov ese
//
// Helpers para Trabajar con CONST + sus valores.

namespace {

bool is_const_with_value(const IrFunction &fn, IrValueId vid, int64_t &out) {
    if (vid == IR_NO_VALUE) return false;
    /* Solo CONST inicializa SSA con un imm conocido.  Buscar la instr
     * que produjo @p vid en cualquier bloque. */
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.dst == vid && ins.op == IrOp::CONST) {
                out = static_cast<int64_t>(ins.imm);
                return true;
            }
        }
    }
    return false;
}

/** @brief Mascara de bits para truncar a @p type. */
uint64_t type_mask(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 0xFFu;
        case IrType::I16: case IrType::U16: return 0xFFFFu;
        case IrType::I32: case IrType::U32: case IrType::F32: return 0xFFFFFFFFu;
        default: return ~static_cast<uint64_t>(0u);
    }
}

bool type_is_signed_int(IrType t) {
    return t == IrType::I8 || t == IrType::I16
        || t == IrType::I32 || t == IrType::I64;
}

/** @brief Sign-extiende @p v desde @p from_t a 64-bit. */
int64_t sign_extend_from(int64_t v, IrType from_t) {
    switch (from_t) {
        case IrType::I8:  return static_cast<int8_t>(v);
        case IrType::I16: return static_cast<int16_t>(v);
        case IrType::I32: return static_cast<int32_t>(v);
        default: return v;
    }
}

/** @brief Sustituye @p ins por un MOV de @p src_vid (mantiene dst).
 *  Devuelve true. */
void rewrite_as_mov(IrInstr &ins, IrValueId src_vid) {
    ins.op = IrOp::MOV;
    ins.operands.clear();
    ins.operands.push_back(src_vid);
    ins.phi_args.clear();
    ins.imm = 0;
    ins.func_name.clear();
    ins.func_ptr = IR_NO_VALUE;
    ins.target_block = IR_NO_BLOCK;
    ins.false_block = IR_NO_BLOCK;
}

/** @brief Convierte @p ins en un CONST con valor @p imm.  No cambia tipo. */
void rewrite_as_const(IrInstr &ins, uint64_t imm) {
    ins.op = IrOp::CONST;
    ins.imm = imm;
    ins.operands.clear();
    ins.phi_args.clear();
    ins.func_name.clear();
    ins.func_ptr = IR_NO_VALUE;
    ins.target_block = IR_NO_BLOCK;
    ins.false_block = IR_NO_BLOCK;
}

} // namespace

bool ir_pass_simplify(IrFunction &fn) {
    bool changed = false;

    /* Pre-build: vid -> CONST imm (si lo es).  Evita el escaneo lineal
     * dentro del bucle principal. */
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }
    auto get_const = [&](IrValueId v, int64_t &out) -> bool {
        if (v == IR_NO_VALUE) return false;
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        out = it->second;
        return true;
    };

    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            switch (ins.op) {
                /* ---- (A) Algebraic identities ---- */
                case IrOp::ADD: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (get_const(ins.operands[0], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[1]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::SUB: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_const(ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::MUL: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c)) {
                        if (c == 1) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        } else if (c == 0) {
                            rewrite_as_const(ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 1) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == 0) {
                            rewrite_as_const(ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        }
                    }
                    break;
                }
                case IrOp::DIV: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* x / 1 = x */
                    if (get_const(ins.operands[1], c) && c == 1) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        /* x / x = 1 (cuidado con x = 0 -> division by zero,
                         * pero el codigo SSA implica que ya se ha dividido,
                         * asi que x != 0; seguro reescribir como const 1) */
                        rewrite_as_const(ins, 1);
                        const_vids[ins.dst] = 1;
                        changed = true;
                    }
                    break;
                }
                case IrOp::MOD: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* x % 1 = 0 */
                    if (get_const(ins.operands[1], c) && c == 1) {
                        rewrite_as_const(ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        /* x % x = 0 (x != 0 implicito) */
                        rewrite_as_const(ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::AND: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    /* commutative: probar ambos operandos */
                    if (get_const(ins.operands[1], c)) {
                        if (c == 0) {
                            rewrite_as_const(ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_const(ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        }
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::OR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c)) {
                        if (c == 0) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_const(ins, static_cast<uint64_t>(-1));
                            const_vids[ins.dst] = -1;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_const(ins, static_cast<uint64_t>(-1));
                            const_vids[ins.dst] = -1;
                            changed = true;
                        }
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }
                case IrOp::XOR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    } else if (get_const(ins.operands[0], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[1]);
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        rewrite_as_const(ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    }
                    break;
                }
                case IrOp::SHL:
                case IrOp::SHR:
                case IrOp::SAR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c = 0;
                    if (get_const(ins.operands[1], c) && c == 0) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                    }
                    break;
                }

                /* ---- (B) Cast de constantes ---- */
                case IrOp::SEXT: {
                    if (ins.operands.empty()) break;
                    /* Identidad: si src_type == dst_type, es un MOV. */
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        int64_t ext = sign_extend_from(c, from_t);
                        rewrite_as_const(ins, static_cast<uint64_t>(ext));
                        const_vids[ins.dst] = ext;
                        changed = true;
                    }
                    break;
                }
                case IrOp::ZEXT: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        uint64_t z = static_cast<uint64_t>(c) & type_mask(from_t);
                        rewrite_as_const(ins, z);
                        const_vids[ins.dst] = static_cast<int64_t>(z);
                        changed = true;
                    }
                    break;
                }
                case IrOp::TRUNC: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        uint64_t m = type_mask(ins.type);
                        uint64_t v = static_cast<uint64_t>(c) & m;
                        /* Si el tipo destino es signed, sign-extender de
                         * vuelta a i64 para preservar el valor logico. */
                        int64_t out_val;
                        if (type_is_signed_int(ins.type)) {
                            out_val = sign_extend_from(static_cast<int64_t>(v), ins.type);
                        } else {
                            out_val = static_cast<int64_t>(v);
                        }
                        rewrite_as_const(ins, static_cast<uint64_t>(out_val));
                        const_vids[ins.dst] = out_val;
                        changed = true;
                    }
                    break;
                }
                case IrOp::BITCAST:
                case IrOp::CAST: {
                    if (ins.operands.empty()) break;
                    IrType from_t = (ins.operands[0] < fn.values.size())
                                    ? fn.values[ins.operands[0]].type
                                    : IrType::I64;
                    if (from_t == ins.type) {
                        rewrite_as_mov(ins, ins.operands[0]);
                        changed = true;
                        break;
                    }
                    int64_t c = 0;
                    if (get_const(ins.operands[0], c)) {
                        rewrite_as_const(ins, static_cast<uint64_t>(c));
                        const_vids[ins.dst] = c;
                        changed = true;
                    }
                    break;
                }

                /* ---- (C) Phi simplification ---- */
                case IrOp::PHI: {
                    if (ins.phi_args.empty()) break;
                    /* Recolectar VIDs unicos (excluyendo self-ref). */
                    IrValueId unique = IR_NO_VALUE;
                    bool multi = false;
                    for (const auto &arg : ins.phi_args) {
                        if (arg.value == IR_NO_VALUE) continue;
                        if (arg.value == ins.dst) continue;  /* self-ref */
                        if (unique == IR_NO_VALUE) {
                            unique = arg.value;
                        } else if (unique != arg.value) {
                            multi = true;
                            break;
                        }
                    }
                    if (!multi && unique != IR_NO_VALUE) {
                        /* Todos los args no-self apuntan al mismo VID. */
                        rewrite_as_mov(ins, unique);
                        changed = true;
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_strength_reduction (Phase D.7.opt)
// =========================================================================
//
// Reemplaza operaciones MUL/DIV/MOD por constante potencia-de-2 con
// shifts/AND, que son significativamente mas baratas:
//   x * 2^k        -> x << k
//   x / 2^k (uN)   -> x >> k       (unsigned: shift logico)
//   x / 2^k (iN)   -> x >> k       (sar para preservar signo)
//   x % 2^k (uN)   -> x & (2^k-1)  (unsigned solo, signed tiene round-bias)
//
// Beneficios:
//   - JIT: skip IMUL (3-5 ciclos) / IDIV (~20-30 ciclos) -> SHL/SHR (1 ciclo).
//   - port-C: el compilador C tambien hace esto pero IR limpio ayuda.
//
// La identificacion de "power of 2" usa popcount (== 1).

namespace {

/** @brief Si @p v es potencia de 2 positiva, devuelve log2(v).  Sino -1. */
int log2_if_power_of_two(uint64_t v) {
    if (v == 0) return -1;
    if ((v & (v - 1)) != 0) return -1;  /* mas de un bit set */
    /* Encontrar el bit set.  Usar __builtin_ctzll si disponible. */
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int k = 0;
    while ((v & 1) == 0) { v >>= 1; ++k; }
    return k;
#endif
}

} // namespace

bool ir_pass_strength_reduction(IrFunction &fn) {
    bool changed = false;

    /* Pre-build vid -> CONST imm. */
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }
    auto get_const_pos = [&](IrValueId v, uint64_t &out) -> bool {
        if (v == IR_NO_VALUE) return false;
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        if (it->second <= 0) return false;  /* solo positivos para SR */
        out = static_cast<uint64_t>(it->second);
        return true;
    };

    /* Crea un nuevo SSA value de tipo CONST y devuelve (new_vid,
     * const_instr).  El caller debe INSERTAR la instr en algun bloque
     * para que el IR sea bien-formado.  Tipicamente justo antes del
     * uso (en el mismo bloque). */
    auto make_new_const = [&](IrType type, uint64_t imm)
                          -> std::pair<IrValueId, IrInstr> {
        const IrValueId new_id = static_cast<IrValueId>(fn.values.size());
        IrValue v{};
        v.id   = new_id;
        v.type = type;
        v.name = "%sr" + std::to_string(new_id);
        v.is_const = true;
        v.const_val = imm;
        fn.values.push_back(v);
        const_vids[new_id] = static_cast<int64_t>(imm);

        IrInstr ci{};
        ci.op   = IrOp::CONST;
        ci.type = type;
        ci.dst  = new_id;
        ci.imm  = imm;
        return {new_id, ci};
    };

    /* Recolectar (bb, pos) -> [list of CONST instrs a insertar ANTES].
     * Lo hacemos en una segunda pasada para no invalidar indices durante
     * el iteracion principal. */
    struct Insertion { size_t bb_idx; size_t pos; IrInstr instr; };
    std::vector<Insertion> pending_insertions;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            IrInstr &ins = bb.instrs[i];
            if (ins.operands.size() < 2) continue;

            switch (ins.op) {
                case IrOp::MUL: {
                    uint64_t cv = 0;
                    IrValueId rhs_const = IR_NO_VALUE;
                    IrValueId other = IR_NO_VALUE;
                    if (get_const_pos(ins.operands[1], cv)) {
                        rhs_const = ins.operands[1];
                        other     = ins.operands[0];
                    } else if (get_const_pos(ins.operands[0], cv)) {
                        rhs_const = ins.operands[0];
                        other     = ins.operands[1];
                    }
                    if (rhs_const == IR_NO_VALUE) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    auto p = make_new_const(IrType::I64,
                        static_cast<uint64_t>(k));
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::SHL;
                    ins.operands = {other, p.first};
                    changed = true;
                    break;
                }
                case IrOp::DIV: {
                    if (ins.type != IrType::U8 && ins.type != IrType::U16
                     && ins.type != IrType::U32 && ins.type != IrType::U64) {
                        break;
                    }
                    uint64_t cv = 0;
                    if (!get_const_pos(ins.operands[1], cv)) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    auto p = make_new_const(IrType::I64,
                        static_cast<uint64_t>(k));
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::SHR;
                    ins.operands = {ins.operands[0], p.first};
                    changed = true;
                    break;
                }
                case IrOp::MOD: {
                    if (ins.type != IrType::U8 && ins.type != IrType::U16
                     && ins.type != IrType::U32 && ins.type != IrType::U64) {
                        break;
                    }
                    uint64_t cv = 0;
                    if (!get_const_pos(ins.operands[1], cv)) break;
                    int k = log2_if_power_of_two(cv);
                    if (k <= 0) break;
                    const uint64_t mask = cv - 1;
                    auto p = make_new_const(ins.type, mask);
                    pending_insertions.push_back({bi, i, p.second});
                    ins.op = IrOp::AND;
                    ins.operands = {ins.operands[0], p.first};
                    changed = true;
                    break;
                }
                default:
                    break;
            }
        }
    }

    /* Aplicar inserciones en orden inverso para no invalidar pos. */
    std::sort(pending_insertions.begin(), pending_insertions.end(),
        [](const Insertion &a, const Insertion &b) {
            if (a.bb_idx != b.bb_idx) return a.bb_idx > b.bb_idx;
            return a.pos > b.pos;
        });
    for (const auto &ins_req : pending_insertions) {
        auto &bb = fn.blocks[ins_req.bb_idx];
        bb.instrs.insert(bb.instrs.begin() + ins_req.pos, ins_req.instr);
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_reassoc
// =========================================================================
//
// Reasocia operaciones binarias asociativas para combinar constantes:
//   (x + c1) + c2  ->  x + (c1+c2)        donde c1+c2 se folda
//   (x * c1) * c2  ->  x * (c1*c2)
//   Idem para AND/OR/XOR.
//
// Permite que const-fold colapse cadenas de operaciones que el const-fold
// local no veria (por no estar en la misma instruccion).
//
// Implementacion: para cada binop con rhs CONST, mira si el lhs es la
// MISMA op con un CONST rhs.  Si si, fusiona y deja la nueva instr.

bool ir_pass_reassoc(IrFunction &fn) {
    bool changed = false;

    /* Build vid -> instr (defining instr) para reassoc lookups. */
    struct DefInfo { IrBlockId bb; size_t idx; };
    std::unordered_map<IrValueId, DefInfo> defs;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            const auto &ins = bb.instrs[i];
            if (ins.dst != IR_NO_VALUE) {
                defs[ins.dst] = {static_cast<IrBlockId>(bi), i};
            }
        }
    }
    std::unordered_map<IrValueId, int64_t> const_vids;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE) {
                const_vids[ins.dst] = static_cast<int64_t>(ins.imm);
            }
        }
    }

    auto is_const = [&](IrValueId v, int64_t &out) -> bool {
        auto it = const_vids.find(v);
        if (it == const_vids.end()) return false;
        out = it->second;
        return true;
    };

    auto make_new_const = [&](IrType type, uint64_t imm)
                          -> std::pair<IrValueId, IrInstr> {
        const IrValueId new_id = static_cast<IrValueId>(fn.values.size());
        IrValue v{};
        v.id = new_id; v.type = type;
        v.name = "%ra" + std::to_string(new_id);
        v.is_const = true; v.const_val = imm;
        fn.values.push_back(v);
        const_vids[new_id] = static_cast<int64_t>(imm);

        IrInstr ci{};
        ci.op   = IrOp::CONST;
        ci.type = type;
        ci.dst  = new_id;
        ci.imm  = imm;
        return {new_id, ci};
    };

    auto is_assoc = [](IrOp op) {
        return op == IrOp::ADD || op == IrOp::MUL
            || op == IrOp::AND || op == IrOp::OR || op == IrOp::XOR;
    };

    auto fold_consts = [](IrOp op, int64_t a, int64_t b, int64_t &out) -> bool {
        switch (op) {
            case IrOp::ADD: out = a + b; return true;
            case IrOp::MUL: out = a * b; return true;
            case IrOp::AND: out = a & b; return true;
            case IrOp::OR:  out = a | b; return true;
            case IrOp::XOR: out = a ^ b; return true;
            default: return false;
        }
    };

    struct ReassocInsert { size_t bb_idx; size_t pos; IrInstr instr; };
    std::vector<ReassocInsert> pending;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &bb = fn.blocks[bi];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            IrInstr &ins = bb.instrs[i];
            if (!is_assoc(ins.op) || ins.operands.size() < 2) continue;
            int64_t c2 = 0;
            IrValueId lhs = ins.operands[0];
            IrValueId rhs = ins.operands[1];
            /* Normalizar: queremos `(x op c1) op c2` con c2 en RHS. */
            if (is_const(lhs, c2) && !is_const(rhs, c2)) {
                std::swap(lhs, rhs);
            }
            if (!is_const(rhs, c2)) continue;
            auto dit = defs.find(lhs);
            if (dit == defs.end()) continue;
            if (dit->second.bb >= fn.blocks.size()) continue;
            const auto &inner_bb = fn.blocks[dit->second.bb];
            if (dit->second.idx >= inner_bb.instrs.size()) continue;
            const IrInstr &inner = inner_bb.instrs[dit->second.idx];
            if (inner.op != ins.op || inner.operands.size() < 2) continue;
            int64_t c1 = 0;
            IrValueId x = inner.operands[0];
            IrValueId rhs_inner = inner.operands[1];
            if (is_const(x, c1) && !is_const(rhs_inner, c1)) {
                std::swap(x, rhs_inner);
            }
            if (!is_const(rhs_inner, c1)) continue;
            int64_t combined = 0;
            if (!fold_consts(ins.op, c1, c2, combined)) continue;
            auto p = make_new_const(ins.type, static_cast<uint64_t>(combined));
            pending.push_back({bi, i, p.second});
            ins.operands = {x, p.first};
            changed = true;
        }
    }
    std::sort(pending.begin(), pending.end(),
        [](const ReassocInsert &a, const ReassocInsert &b) {
            if (a.bb_idx != b.bb_idx) return a.bb_idx > b.bb_idx;
            return a.pos > b.pos;
        });
    for (const auto &ins_req : pending) {
        auto &bb = fn.blocks[ins_req.bb_idx];
        bb.instrs.insert(bb.instrs.begin() + ins_req.pos, ins_req.instr);
    }
    return changed;
}

bool ir_pass_dce(IrFunction &fn) {
    // Construir conjunto de valores que son usados en algun operando
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            // CALLIND y CALLCLOSURE referencian el callee via func_ptr (no
            // via operands), asi que DCE debe contarlo como uso para que el
            // SSA value que produjo el puntero (e.g. RAW_ASM `mov rN, @Abs(...)`
            // o LOAD del slot del function value) NO sea eliminado.  Sin
            // esto, las closures se rompen: el optimizer purga la instr que
            // materializa fn_addr y el regalloc deja r14 (asignado a
            // fn_addr_v) sin inicializar -> callvmr salta a 0 y crash.
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            // Una instruccion con resultado no usado y sin efectos laterales se elimina,
            // EXCEPTO si lleva el flag @c preserve (barreras del codegen).
            if (ins.dst != IR_NO_VALUE
                && !used.count(ins.dst)
                && !is_side_effecting(ins.op)
                && !ins.preserve) {
                keep  = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase de propagacion de copias
// =========================================================================

bool ir_pass_copy_prop(IrFunction &fn) {
    // Construir mapa de sustituciones: %b -> %a para cada "%b = mov %a"
    std::unordered_map<IrValueId, IrValueId> subst;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::MOV
                && !ins.preserve
                && ins.dst != IR_NO_VALUE
                && ins.operands.size() == 1
                && ins.operands[0] != IR_NO_VALUE) {
                // seguir la cadena de sustituciones
                IrValueId src = ins.operands[0];
                while (subst.count(src)) src = subst[src];
                subst[ins.dst] = src;
            }
        }
    }
    if (subst.empty()) return false;

    // Aplicar sustituciones en todos los operandos
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            // sustituir operandos normales
            for (auto &op : ins.operands) {
                auto it = subst.find(op);
                if (it != subst.end() && it->second != op) {
                    op      = it->second;
                    changed = true;
                }
            }
            // sustituir func_ptr en CALLIND
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = subst.find(ins.func_ptr);
                if (it != subst.end() && it->second != ins.func_ptr) {
                    ins.func_ptr = it->second;
                    changed      = true;
                }
            }
            // sustituir argumentos phi
            for (auto &pa : ins.phi_args) {
                auto it = subst.find(pa.value);
                if (it != subst.end() && it->second != pa.value) {
                    pa.value = it->second;
                    changed  = true;
                }
            }
        }
    }
    // Eliminar los MOV que ahora son copias triviales (%a = mov %a)
    if (changed) ir_pass_dce(fn);
    return changed;
}

// =========================================================================
//  Pase de plegado de constantes
// =========================================================================

/** @brief Devuelve el valor constante de un IrValue, o UNDEF si no es const. */
static bool get_const(const IrFunction &fn, IrValueId id, uint64_t &val) {
    if (id == IR_NO_VALUE || id >= static_cast<IrValueId>(fn.values.size()))
        return false;
    const IrValue &v = fn.values[id];
    if (!v.is_const) return false;
    val = v.const_val;
    return true;
}

bool ir_pass_const_fold(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.dst == IR_NO_VALUE) continue;
            if (!is_pure(ins.op))       continue;

            // --- Operaciones binarias enteras ---
            if (ins.operands.size() == 2) {
                uint64_t a, b;
                bool ca = get_const(fn, ins.operands[0], a);
                bool cb = get_const(fn, ins.operands[1], b);
                if (!ca || !cb) continue;

                uint64_t res = 0;
                bool folded  = true;
                int64_t sa = static_cast<int64_t>(a);
                int64_t sb = static_cast<int64_t>(b);

                switch (ins.op) {
                    case IrOp::ADD:     res = a + b;                        break;
                    case IrOp::SUB:     res = a - b;                        break;
                    case IrOp::MUL:     res = a * b;                        break;
                    case IrOp::DIV:     res = (sb != 0) ? (uint64_t)(sa / sb) : 0; break;
                    case IrOp::MOD:     res = (sb != 0) ? (uint64_t)(sa % sb) : 0; break;
                    case IrOp::AND:     res = a & b;                        break;
                    case IrOp::OR:      res = a | b;                        break;
                    case IrOp::XOR:     res = a ^ b;                        break;
                    case IrOp::SHL:     res = a << (b & 63);                break;
                    case IrOp::SHR:     res = a >> (b & 63);                break;
                    case IrOp::SAR:     res = (uint64_t)(sa >> (b & 63));   break;
                    // comparaciones enteras -> resultado bool (0 o 1)
                    case IrOp::CMP_EQ:  res = (a == b)           ? 1 : 0;  break;
                    case IrOp::CMP_NE:  res = (a != b)           ? 1 : 0;  break;
                    case IrOp::CMP_LT:  res = (sa <  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GT:  res = (sa >  sb)         ? 1 : 0;  break;
                    case IrOp::CMP_LE:  res = (sa <= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_GE:  res = (sa >= sb)         ? 1 : 0;  break;
                    case IrOp::CMP_ULT: res = (a <  b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGT: res = (a >  b)           ? 1 : 0;  break;
                    case IrOp::CMP_ULE: res = (a <= b)           ? 1 : 0;  break;
                    case IrOp::CMP_UGE: res = (a >= b)           ? 1 : 0;  break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op         = IrOp::CONST;
                    ins.imm        = res;
                    ins.operands.clear();
                    ins.type       = (ins.op == IrOp::CMP_EQ) ? IrType::BOOL : ins.type;
                    // marcar el valor destino como constante
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }

            // --- Operaciones unarias enteras ---
            if (ins.operands.size() == 1) {
                uint64_t a;
                if (!get_const(fn, ins.operands[0], a)) continue;

                uint64_t res  = 0;
                bool folded   = true;
                int64_t sa = static_cast<int64_t>(a);

                switch (ins.op) {
                    case IrOp::NEG:     res = (uint64_t)(-sa);  break;
                    case IrOp::NOT:     res = ~a;                break;
                    case IrOp::ZEXT:    res = a;                 break;
                    case IrOp::TRUNC:   res = a & 0xFFFFFFFFULL; break;
                    case IrOp::MOV:     res = a;                 break;
                    default: folded = false; break;
                }

                if (folded) {
                    ins.op  = IrOp::CONST;
                    ins.imm = res;
                    ins.operands.clear();
                    if (ins.dst < static_cast<IrValueId>(fn.values.size())) {
                        fn.values[ins.dst].is_const  = true;
                        fn.values[ins.dst].const_val = res;
                    }
                    changed = true;
                }
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase de eliminacion de bloques inalcanzables
// =========================================================================

// =========================================================================
//  Pase Dead Store Elimination
// =========================================================================
//
// Para cada bloque basico, detecta STOREs sucesivos a la MISMA direccion
// sin lecturas intermedias.  El primer STORE es muerto: su valor sera
// sobrescrito sin ser leido.
//
// Es conservador con side-effects: cualquier CALL/RAW_ASM/CALLN limpia el
// estado (no podemos garantizar que el callee no lea la memoria).
//
// Patron comun en frontend Vex:
//   %a = const.i64 0
//   store %a, %slot
//   ...   (sin LOAD de %slot, sin CALL)
//   %b = const.i64 42
//   store %b, %slot   <-- valor que persiste; el store anterior es dead
//
// Bajo esto a IR:
//   - El primer STORE se marca para eliminar
//   - La CONST que solo alimentaba al store dead queda dead -> DCE la quita
//
// Ahorro: en codigo generado por frontend Vex se ven STOREs de zero seguidos
// de STOREs reales (init list, alloca cleared, etc).  ~10-15% reduccion.
bool ir_pass_dse(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        // Mapa: ptr_vid -> indice del ultimo STORE a ese ptr (en este bloque)
        std::unordered_map<IrValueId, size_t> last_store_idx;
        // Phase D.7.opt: STORE-TO-LOAD FORWARDING.
        // Mapa paralelo: ptr_vid -> (stored_value_vid, store_type) del
        // ultimo STORE.  Cuando un LOAD lee de ese mismo ptr CON EL MISMO
        // tipo, podemos reemplazar el LOAD por MOV del valor almacenado
        // (ahorra la lectura de memoria + cualquier conversion).
        std::unordered_map<IrValueId, std::pair<IrValueId, IrType>> last_store_val;
        // Set de indices marcados como dead
        std::vector<bool> dead(bb.instrs.size(), false);

        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            auto &ins = bb.instrs[i];
            switch (ins.op) {
                case IrOp::STORE: {
                    if (ins.operands.size() < 2) break;
                    IrValueId ptr = ins.operands[1];
                    IrValueId val = ins.operands[0];
                    if (ptr == IR_NO_VALUE) break;
                    auto it = last_store_idx.find(ptr);
                    if (it != last_store_idx.end()) {
                        // STORE anterior al mismo ptr SIN reads intermedios.
                        // El anterior es DEAD.
                        dead[it->second] = true;
                        changed = true;
                    }
                    last_store_idx[ptr] = i;
                    if (val != IR_NO_VALUE) {
                        last_store_val[ptr] = {val, ins.type};
                    }
                    break;
                }
                case IrOp::LOAD: {
                    if (ins.operands.empty()) break;
                    IrValueId ptr = ins.operands[0];
                    if (ptr == IR_NO_VALUE) break;
                    auto it = last_store_val.find(ptr);
                    if (it != last_store_val.end() && it->second.second == ins.type) {
                        // SLF: reemplazar LOAD por MOV del valor almacenado.
                        // copy_prop posterior substituye los usos del LOAD dst
                        // por el stored value y DCE elimina el MOV.
                        //
                        // Bug fix is_host_ptr propagation: el LOAD dst pudo
                        // estar marcado is_host_ptr=true por el frontend (e.g.
                        // lend() lowering marca el load del slot[0] de un
                        // unique<T> como host_ptr).  Al substituir uses con
                        // el stored value, ESE value debe tambien tener
                        // is_host_ptr=true para que LOAD/STORE posteriores
                        // emitan movh (host mem) en vez de mov (VM mem).
                        // Semanticamente: load(store(v, p)) == v, asi que
                        // el LOAD result Y el stored value son el MISMO
                        // value runtime y deben compartir flags.
                        if (ins.dst < fn.values.size()
                         && it->second.first < fn.values.size()) {
                            const auto &dst_v = fn.values[ins.dst];
                            auto &val_v = fn.values[it->second.first];
                            if (dst_v.is_host_ptr && !val_v.is_host_ptr) {
                                val_v.is_host_ptr = true;
                            }
                            if (dst_v.is_gc_object && !val_v.is_gc_object) {
                                val_v.is_gc_object = true;
                            }
                            if (dst_v.pointee_is_host_ptr && !val_v.pointee_is_host_ptr) {
                                val_v.pointee_is_host_ptr = true;
                            }
                        }
                        ins.op = IrOp::MOV;
                        ins.operands = {it->second.first};
                        changed = true;
                    }
                    // No invalidar last_store_idx: el STORE no es dead
                    // (acabamos de demostrar que ESTE LOAD lo lee).
                    last_store_idx.erase(ptr);
                    break;
                }
                case IrOp::ARRAY_LOAD:
                case IrOp::GETFIELD:
                case IrOp::ARRAY_LEN:
                    // Cualquier LOAD desde un ptr que tenemos seguido invalida
                    // la posibilidad de eliminar el STORE previo (no sabemos
                    // alias).  Conservador: limpiar todo el mapa.
                    last_store_idx.clear();
                    last_store_val.clear();
                    break;
                // Side-effects/calls: limpiar (memoria puede cambiar dentro).
                case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
                case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
                case IrOp::TAILCALL:
                case IrOp::RAW_ASM:
                case IrOp::MEMCPY: case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
                case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
                case IrOp::NEWOBJ: case IrOp::GC_ALLOC:
                case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
                case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
                    last_store_idx.clear();
                    last_store_val.clear();
                    break;
                default:
                    break;
            }
        }

        // Compactar: eliminar instrucciones marcadas dead.
        if (changed) {
            std::vector<IrInstr> kept;
            kept.reserve(bb.instrs.size());
            for (size_t i = 0; i < bb.instrs.size(); ++i) {
                if (!dead[i]) kept.push_back(std::move(bb.instrs[i]));
            }
            bb.instrs = std::move(kept);
        }
    }

    // DCE para limpiar valores que solo alimentaban los stores eliminados,
    // y copy_prop para resolver los MOVs generados por SLF.
    if (changed) {
        ir_pass_copy_prop(fn);
        ir_pass_dce(fn);
    }
    return changed;
}

bool ir_pass_unreachable(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    const size_t nblocks = fn.blocks.size();
    std::vector<bool> reachable(nblocks, false);

    // BFS desde el bloque de entrada (bloque 0)
    std::queue<IrBlockId> worklist;
    worklist.push(0);
    reachable[0] = true;

    while (!worklist.empty()) {
        IrBlockId bid = worklist.front();
        worklist.pop();

        if (bid >= nblocks) continue;
        const IrBlock &bb = fn.blocks[bid];

        // Encontrar el terminador del bloque y sus sucesores
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::BR) {
                IrBlockId s = ins.target_block;
                if (s < nblocks && !reachable[s]) {
                    reachable[s] = true;
                    worklist.push(s);
                }
            } else if (ins.op == IrOp::BR_COND) {
                for (IrBlockId s : {ins.target_block, ins.false_block}) {
                    if (s < nblocks && !reachable[s]) {
                        reachable[s] = true;
                        worklist.push(s);
                    }
                }
            }
        }
        // Usar sucesores precalculados si existen
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && !reachable[s]) {
                reachable[s] = true;
                worklist.push(s);
            }
        }
    }

    // Verificar si hay bloques inalcanzables
    bool any_unreachable = false;
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) { any_unreachable = true; break; }
    }
    if (!any_unreachable) return false;

    // Construir mapa de reindexacion: viejo_id -> nuevo_id
    std::vector<IrBlockId> remap(nblocks, IR_NO_BLOCK);
    IrBlockId new_id = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        if (reachable[b]) remap[b] = new_id++;
    }

    // Reescribir los bloques: eliminar los inalcanzables y actualizar referencias
    std::vector<IrBlock> new_blocks;
    new_blocks.reserve(new_id);
    for (size_t b = 0; b < nblocks; ++b) {
        if (!reachable[b]) continue;
        IrBlock bb = fn.blocks[b];
        bb.id = remap[b];

        // Actualizar predecesores y sucesores
        std::vector<IrBlockId> new_preds, new_succs;
        for (IrBlockId p : bb.preds) {
            if (p < nblocks && reachable[p]) new_preds.push_back(remap[p]);
        }
        for (IrBlockId s : bb.succs) {
            if (s < nblocks && reachable[s]) new_succs.push_back(remap[s]);
        }
        bb.preds = std::move(new_preds);
        bb.succs = std::move(new_succs);

        // Actualizar referencias de bloques en instrucciones
        for (auto &ins : bb.instrs) {
            if (ins.target_block != IR_NO_BLOCK && ins.target_block < nblocks)
                ins.target_block = remap[ins.target_block];
            if (ins.false_block != IR_NO_BLOCK && ins.false_block < nblocks)
                ins.false_block = remap[ins.false_block];
            // Eliminar argumentos phi que vienen de bloques inalcanzables
            std::vector<IrPhiArg> new_phi;
            for (const auto &pa : ins.phi_args) {
                if (pa.block < nblocks && reachable[pa.block]) {
                    IrPhiArg na = pa;
                    na.block = remap[pa.block];
                    new_phi.push_back(na);
                }
            }
            ins.phi_args = std::move(new_phi);
        }
        new_blocks.push_back(std::move(bb));
    }
    fn.blocks = std::move(new_blocks);
    return true;
}

// =========================================================================
//  Pase CSE (Common Subexpression Elimination, local)
// =========================================================================

/* Global const CSE: deduplicar CONSTs en bloques no-entry contra los
 * de entry (que dominan a todos los demas).  Pase separado, seguro a O2.
 *
 * Implementacion: en vez de rewrite-CONST-to-MOV-then-copy-prop (que
 * dejaba el is_const flag stale en fn.values y causaba interaccion mala
 * con simplify/const_fold), hacemos un DIRECT REWRITE:
 *
 *   1. Recolectar mapa {(type, imm) -> entry_vid} de entry.
 *   2. Recolectar mapa {dup_vid -> canon_vid} para CONSTs duplicados en
 *      bloques no-entry.
 *   3. Aplicar sustitucion en TODOS los operandos / func_ptr / phi_args.
 *   4. Eliminar las instrucciones CONST duplicadas (no las convertimos a
 *      MOV; las quitamos del bloque directamente -- DCE no es necesario).
 *
 * Esto evita el camino MOV+copy_prop que dejaba is_const stale.  La
 * IrValue del dup_vid queda huerfana (sin instr definidora), pero como
 * todos sus usos se substituyen, no se referencia mas. */
bool ir_pass_const_cse_entry(IrFunction &fn) {
    if (fn.blocks.empty()) return false;
    /* Pase 1: en entry, recolectar el PRIMER vid por (type,imm) y registrar
     * duplicados subsiguientes -> subst (apuntan al primer vid). */
    std::unordered_map<std::string, IrValueId> entry_const_table;
    std::unordered_map<IrValueId, IrValueId> subst;
    for (const auto &ins : fn.blocks[0].instrs) {
        if (ins.op != IrOp::CONST || ins.dst == IR_NO_VALUE) continue;
        std::ostringstream key;
        key << static_cast<int>(ins.type) << ":" << ins.imm;
        std::string k = key.str();
        auto it = entry_const_table.find(k);
        if (it == entry_const_table.end()) {
            entry_const_table[k] = ins.dst;
        } else if (it->second != ins.dst) {
            /* Duplicado dentro de entry -- redirige al primero. */
            subst[ins.dst] = it->second;
        }
    }
    if (entry_const_table.empty()) return false;

    /* Pase 2: en bloques no-entry, identificar duplicados y agregar subst. */
    for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
        for (const auto &ins : fn.blocks[bi].instrs) {
            if (ins.op != IrOp::CONST || ins.dst == IR_NO_VALUE) continue;
            std::ostringstream key;
            key << static_cast<int>(ins.type) << ":" << ins.imm;
            auto it = entry_const_table.find(key.str());
            if (it != entry_const_table.end() && it->second != ins.dst) {
                subst[ins.dst] = it->second;
            }
        }
    }
    if (subst.empty()) return false;

    /* Aplicar sustitucion en operandos / func_ptr / phi_args. */
    auto canon = [&](IrValueId v) -> IrValueId {
        IrValueId cur = v;
        int hops = 0;
        while (subst.count(cur) && hops++ < 8) cur = subst[cur];
        return cur;
    };
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            for (auto &op : ins.operands) {
                IrValueId c = canon(op);
                if (c != op) { op = c; changed = true; }
            }
            if (ins.func_ptr != IR_NO_VALUE) {
                IrValueId c = canon(ins.func_ptr);
                if (c != ins.func_ptr) { ins.func_ptr = c; changed = true; }
            }
            for (auto &pa : ins.phi_args) {
                IrValueId c = canon(pa.value);
                if (c != pa.value) { pa.value = c; changed = true; }
            }
        }
    }

    /* Eliminar las instrucciones CONST duplicadas EN TODOS LOS BLOQUES
     * (incluyendo entry: si entry tiene dups internos, los quitamos). */
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto &instrs = fn.blocks[bi].instrs;
        std::vector<IrInstr> kept;
        kept.reserve(instrs.size());
        for (auto &ins : instrs) {
            if (ins.op == IrOp::CONST && ins.dst != IR_NO_VALUE
             && subst.count(ins.dst)) {
                /* skip: duplicado eliminado */
                changed = true;
            } else {
                kept.push_back(std::move(ins));
            }
        }
        instrs = std::move(kept);
    }
    return changed;
}

bool ir_pass_cse(IrFunction &fn) {
    bool changed = false;

    /* ============================================================
     * GLOBAL const CSE.
     *
     * Antes del CSE local per-block, deduplicar CONSTs cross-block
     * recolectando el PRIMER CONST por (type, imm) en orden lineal de
     * bloques y registrando un mapa global de sustitucion.  Como el
     * bloque 0 (entry) domina a todos los demas, si el primer CONST
     * esta en entry, todos los duplicados en bloques posteriores
     * pueden referirse a el sin violar SSA dominance.
     *
     * Para CONSTs no en entry: solo deduplicamos si el primer CONST
     * encontrado en orden lineal de bloques domina a los duplicados.
     * Aproximacion conservadora: solo dedupe si el primer CONST esta
     * en el bloque 0 (entry).  Mas casos requeririan dominator analysis,
     * pero como SR/reassoc/LICM insertan CONSTs en entry por construccion,
     * esta heuristica cubre el 95% de los casos reales.
     * ============================================================ */
    {
        std::unordered_map<std::string, IrValueId> entry_const_table;
        /* Pase 1: recolectar CONSTs en entry (bloque 0). */
        if (!fn.blocks.empty()) {
            for (const auto &ins : fn.blocks[0].instrs) {
                if (ins.op != IrOp::CONST) continue;
                if (ins.dst == IR_NO_VALUE) continue;
                std::ostringstream key;
                key << static_cast<int>(ins.type) << ":" << ins.imm;
                std::string k = key.str();
                if (!entry_const_table.count(k)) {
                    entry_const_table[k] = ins.dst;
                }
            }
        }
        /* Pase 2: en otros bloques, deduplicar CONSTs cuyo (type, imm)
         * matchea uno de entry.  Convertir a MOV. */
        if (!entry_const_table.empty()) {
            for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
                for (auto &ins : fn.blocks[bi].instrs) {
                    if (ins.op != IrOp::CONST) continue;
                    if (ins.dst == IR_NO_VALUE) continue;
                    std::ostringstream key;
                    key << static_cast<int>(ins.type) << ":" << ins.imm;
                    auto it = entry_const_table.find(key.str());
                    if (it != entry_const_table.end() && it->second != ins.dst) {
                        ins.op = IrOp::MOV;
                        ins.operands = {it->second};
                        ins.imm = 0;
                        changed = true;
                    }
                }
            }
        }
    }
    /* Tras el global const CSE, copy_prop limpiara los MOVs. */

    auto is_mem_read = [](IrOp op) -> bool {
        return op == IrOp::LOAD || op == IrOp::ARRAY_LOAD
            || op == IrOp::GETFIELD || op == IrOp::ARRAY_LEN;
    };

    for (auto &bb : fn.blocks) {
        // Tabla: hash de (op, type, operands) -> IrValueId del primer calculo
        std::unordered_map<std::string, IrValueId> expr_table;
        // Mapa paralelo: clave -> bool (es memory-read?) para invalidar
        // rapido al ver side-effects.
        std::unordered_set<std::string> mem_read_keys;
        // Mapa de sustituciones para aplicar
        std::unordered_map<IrValueId, IrValueId> subst;

        for (auto &ins : bb.instrs) {
            /* Bug fix: instrucciones SIN dst (STORE/BR/RET/CALL-void)
             * pueden tener side-effects que invalidan memory-read entries.
             * Procesar invalidacion ANTES del `continue` por dst==NO_VALUE. */
            if (!is_pure(ins.op)) {
                for (const auto &mk : mem_read_keys) expr_table.erase(mk);
                mem_read_keys.clear();
                continue;
            }
            if (ins.dst == IR_NO_VALUE) continue;
            if (ins.op == IrOp::PHI) continue;  /* phi no se dedupea */

            /* dedupe CONSTs por (type, imm).  Antes el
             * pase ignoraba CONSTs explicitamente; los port targets
             * (e.g. port-C) y el JIT se beneficiaban de tener un solo
             * SSA value por (type, imm).  Reduce el numero de slots
             * stack alocados y el output destino es mas limpio. */
            if (ins.op == IrOp::CONST) {
                std::ostringstream key;
                key << "C:" << static_cast<int>(ins.type) << ":" << ins.imm;
                std::string k = key.str();
                auto it = expr_table.find(k);
                if (it != expr_table.end()) {
                    subst[ins.dst] = it->second;
                    ins.op = IrOp::MOV;
                    ins.operands = {it->second};
                    ins.imm = 0;
                    changed = true;
                } else {
                    expr_table[k] = ins.dst;
                }
                continue;
            }

            // Construir clave canonica: "op:type:imm:op0:op1:..."
            //
            // Bug fix: imm es semanticamente significativo para STR_LIT_ADDR
            // (indice del string), GETFIELD (offset del campo), ALLOCA (size),
            // y posiblemente otros.  Incluirlo en la clave evita dedupe falso
            // (e.g., dos str_lit_addr con strings distintos parecian iguales).
            std::ostringstream key;
            key << static_cast<int>(ins.op) << ":"
                << static_cast<int>(ins.type) << ":"
                << ins.imm;
            for (IrValueId op : ins.operands) {
                // Resolver sustituciones previas en los operandos
                IrValueId canonical = op;
                while (subst.count(canonical)) canonical = subst[canonical];
                key << ":" << canonical;
            }
            std::string k = key.str();

            auto it = expr_table.find(k);
            if (it != expr_table.end()) {
                // Expresion ya calculada: sustituir dst con el valor anterior
                subst[ins.dst] = it->second;
                ins.op  = IrOp::MOV;
                ins.operands = {it->second};
                changed = true;
            } else {
                // Primera ocurrencia: registrarla
                expr_table[k] = ins.dst;
                if (is_mem_read(ins.op)) mem_read_keys.insert(k);
                // Aplicar sustituciones previas a los operandos de esta instruccion
                for (auto &op : ins.operands) {
                    auto sit = subst.find(op);
                    if (sit != subst.end()) op = sit->second;
                }
            }
        }
    }
    if (changed) ir_pass_copy_prop(fn); // limpiar los MOV generados por CSE
    return changed;
}

// =========================================================================
//  Pase TCO (Tail Call Optimization)
// =========================================================================

// =========================================================================
//  Helper: detectar si un IrValueId deriva (transitivamente) de una ALLOCA
//
//  Usado por TCO para descartar la transformacion CALL->TAILCALL cuando
//  algun argumento referencia memoria asignada en el frame del caller.
//  Razon: TAILCALL emite `leave` antes del salto al callee, lo que
//  restaura RSP=RBP y libera el bloque ALLOCA.  Si el callee dereferencia
//  un puntero que apuntaba a esa region, lee basura (o memoria del
//  callee).  Demo regresion: 17_ecs_basico.vex pasaba arrays
//  i32[8] (ALLOCA) por valor a system_sum_positions y obtenia R0=0
//  en vez de 100 con TCO activo.
//
//  La deteccion es conservadora: marcamos un valor como "alloca-derived"
//  si su def es ALLOCA, MEMBER (que devuelve direccion en frame), o
//  cualquier ADD/SUB/MOV/STORE/STR_LIT_ADDR cuyo operando ya este marcado.
//  No intenta tracking flow-sensitive: una sobreestimacion implica
//  perder TCO en ese caller, no incorrectness.
// =========================================================================
static bool collect_alloca_derived(const IrFunction &fn,
                                    std::unordered_set<IrValueId> &out) {
    out.clear();
    // Pase 1: identificar las definiciones ALLOCA directas.
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            if (ins.op == IrOp::ALLOCA && ins.dst != IR_NO_VALUE) {
                out.insert(ins.dst);
            }
        }
    }
    if (out.empty()) return false;

    // Pase 2: propagar la marca por aritmetica de punteros y MOV/PHI.
    // Iteramos hasta punto fijo (cota: numero de blocks * instrs por block).
    bool changed = true;
    int  guard   = 1024;
    while (changed && guard-- > 0) {
        changed = false;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.dst == IR_NO_VALUE) continue;
                if (out.count(ins.dst)) continue;
                bool any_op_tainted = false;
                for (auto op : ins.operands) {
                    if (op != IR_NO_VALUE && out.count(op)) {
                        any_op_tainted = true;
                        break;
                    }
                }
                if (!any_op_tainted) {
                    for (const auto &pa : ins.phi_args) {
                        if (out.count(pa.value)) {
                            any_op_tainted = true;
                            break;
                        }
                    }
                }
                if (any_op_tainted) {
                    // Solo propagamos por ops que SI pueden producir una
                    // direccion derivada: aritmetica (ADD/SUB), copias
                    // (MOV, PHI), o casts/cargas/loads que conserven el
                    // puntero.  Para ALU "verdadera" sobre escalares no
                    // hay riesgo, asi que la lista blanca es restrictiva
                    // pero suficiente para el patron observado.
                    switch (ins.op) {
                        case IrOp::ADD: case IrOp::SUB:
                        case IrOp::MOV: case IrOp::PHI:
                            out.insert(ins.dst);
                            changed = true;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
    return true;
}

bool ir_pass_tailcall(IrFunction &fn) {
    // Detecta el patron: CALL @f(args) seguido inmediatamente de RET %resultado
    // y convierte el CALL en TAILCALL (elimina la RET subsiguiente).
    // Tambien maneja RET void inmediatamente despues de CALL void.
    //
    // SAFETY: NO se promueve a TAILCALL si algun argumento es derivado
    // de una ALLOCA del caller.  TAILCALL emite `leave` (que restaura
    // RSP=RBP y libera el frame), invalidando los punteros que apuntan
    // al area de allocas.  El callee leeria basura.  La deteccion se
    // hace una vez por funcion y la cache se reutiliza para todos los
    // CALLs candidatos.
    bool changed = false;

    std::unordered_set<IrValueId> alloca_derived;
    const bool fn_has_alloca = collect_alloca_derived(fn, alloca_derived);

    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        for (size_t i = 0; i + 1 < instrs.size(); ) {
            IrInstr &call = instrs[i];
            IrInstr &ret  = instrs[i + 1];

            // Solo CALL directo (no CALLVIRT, CALLN, CALLIND)
            if (call.op != IrOp::CALL) { ++i; continue; }
            if (ret.op  != IrOp::RET)  { ++i; continue; }

            // Verificar que RET usa directamente el resultado del CALL (o es void)
            bool ret_uses_call = (!ret.operands.empty()
                                  && ret.operands[0] == call.dst);
            bool ret_is_void   = ret.operands.empty();

            if (!ret_uses_call && !ret_is_void) { ++i; continue; }

            // Bloqueo de seguridad: si CUALQUIER arg es derivado de
            // ALLOCA del caller, NO promover a TAILCALL.  El leave
            // posterior liberaria la memoria todavia referenciada.
            if (fn_has_alloca) {
                bool unsafe = false;
                for (auto op : call.operands) {
                    if (op != IR_NO_VALUE && alloca_derived.count(op)) {
                        unsafe = true;
                        break;
                    }
                }
                if (unsafe) { ++i; continue; }
            }

            // Convertir: CALL -> TAILCALL, eliminar RET
            call.op  = IrOp::TAILCALL;
            call.dst = IR_NO_VALUE; // TAILCALL no tiene destino
            instrs.erase(instrs.begin() + static_cast<ptrdiff_t>(i + 1));
            changed = true;
            // No avanzar 'i': re-examinar la misma posicion por si hay otro patron
        }
    }
    return changed;
}

// =========================================================================
//  Pase inline_loop_header (peephole pre-codegen)
// =========================================================================

/**
 * @brief Inline de header trivial de loop para habilitar fusion decjnz.
 *
 * Detecta el patron:
 *   B: ...; <SUB>; br H
 *   H: %cmp = CMP_X(...); br.cond %cmp, T, F
 * con H teniendo UN SOLO predecesor (B).  Mueve las 2 instrs de H al
 * final de B (reemplazando el br) y vacia H.  Despues, B queda con
 * SUB+CMP+BR_COND consecutivos -> el peephole de same-block del IR
 * emitter aplica decjnz fusion.
 *
 * Generaliza a cualquier patron "header trivial con un predecesor", no
 * solo a decjnz.  Otros peepholes (cmpjmp) tambien se benefician.
 *
 * Coste: O(N_blocks * N_predecessors).  El check de predecesores es
 * lineal pero solo se ejecuta cuando el header tiene exactamente 2
 * instr (raro fuera de loop headers).
 *
 * @return true si se hizo al menos una fusion (puede dispararse otro DCE).
 */
bool ir_pass_inline_loop_header(IrFunction &fn) {
    bool changed = false;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        IrBlock &B = fn.blocks[bi];
        if (B.instrs.empty()) continue;
        IrInstr &term = B.instrs.back();
        if (term.op != IrOp::BR) continue; // solo BR incondicional
        IrBlockId hid = term.target_block;
        if (hid >= fn.blocks.size() || hid == static_cast<IrBlockId>(bi)) continue;
        IrBlock &H = fn.blocks[hid];
        // H debe terminar en BR_COND.  Las instrs pueden incluir CONSTs
        // literales que materializan valores usados solo por el CMP final.
        // Patron tipico: %k = const.i64 0; %z = cmp.ne.bool %x, %k; br.cond %z, T, F.
        // Tambien permitimos el CMP en penultima posicion con 0..N CONSTs antes.
        if (H.instrs.size() < 2) continue;
        const IrInstr &h_last = H.instrs.back();
        if (h_last.op != IrOp::BR_COND) continue;
        // Buscar el CMP penultimo (h_last - 1) o anterior.  Las instrs
        // entre el CMP y el BR_COND deben ser CONST puros (sin side effects).
        size_t cmp_idx = H.instrs.size() - 2;
        const IrInstr &h_cmp = H.instrs[cmp_idx];
        bool is_cmp = (h_cmp.op == IrOp::CMP_EQ  || h_cmp.op == IrOp::CMP_NE
                    || h_cmp.op == IrOp::CMP_LT  || h_cmp.op == IrOp::CMP_GT
                    || h_cmp.op == IrOp::CMP_LE  || h_cmp.op == IrOp::CMP_GE
                    || h_cmp.op == IrOp::CMP_ULT || h_cmp.op == IrOp::CMP_UGT
                    || h_cmp.op == IrOp::CMP_ULE || h_cmp.op == IrOp::CMP_UGE);
        if (!is_cmp) continue;
        // Verificar que las instrs antes del CMP sean todas CONST (puras).
        bool only_consts = true;
        for (size_t k = 0; k < cmp_idx; ++k) {
            if (H.instrs[k].op != IrOp::CONST) { only_consts = false; break; }
        }
        if (!only_consts) continue;
        // El cmp.dst debe ser el unico operand del BR_COND.
        if (h_last.operands.empty() || h_last.operands[0] != h_cmp.dst) continue;
        // Contar predecesores de H.
        int preds = 0;
        for (size_t pi = 0; pi < fn.blocks.size(); ++pi) {
            if (fn.blocks[pi].instrs.empty()) continue;
            const IrInstr &pterm = fn.blocks[pi].instrs.back();
            if (pterm.op == IrOp::BR && pterm.target_block == hid) ++preds;
            else if (pterm.op == IrOp::BR_COND
                  && (pterm.target_block == hid || pterm.false_block == hid))
                ++preds;
        }
        if (preds != 1) continue; // mas de 1 pred o 0 -> no fusionar
        // No tocar entry block: si H es entry, no podemos fusionarlo
        // (entry no tiene predecesores; ya filtrado por preds!=1, pero
        // doble check defensivo).
        if (hid == 0) continue;
        // El header NO debe tener PHI nodes (el primer instr debe ser CMP, no PHI).
        // Ya implicito en H.instrs.size() == 2 con h0 = CMP.

        // Aplicar la fusion:
        //   1. Eliminar BR de B.
        //   2. Append todas las instrs de H (CONSTs + CMP + BR_COND) al final de B.
        //   3. Hoist de CONSTs: mover todas las IrOp::CONST justo despues de
        //      las PHI nodes (CONSTs son puros, su orden es irrelevante para
        //      la semantica).  Esto agrupa los CONSTs y deja a SUB+CMP+BR_COND
        //      consecutivos en el final, habilitando peepholes como decjnz.
        //   4. Limpiar H (queda inalcanzable, lo barren los demas pases).
        //   5. Reescribir phi_args en sucesores de BR_COND: ref a H debe pasar a B.
        IrBlockId t_true  = h_last.target_block;
        IrBlockId t_false = h_last.false_block;

        std::vector<IrInstr> moved;
        moved.reserve(H.instrs.size());
        for (auto &ins : H.instrs) moved.push_back(std::move(ins));
        B.instrs.pop_back(); // remover BR de B
        for (auto &ins : moved) B.instrs.push_back(std::move(ins));
        H.instrs.clear();

        // Hoist de CONSTs: estabilizamos el orden en B asi:
        //   [PHIs...] [CONSTs...] [resto en orden original]
        // Stable_partition mantiene el orden relativo de cada grupo.  Los
        // CONSTs no tienen side effects ni dependen de instrucciones
        // anteriores (su unico operand_id es @c imm), asi que moverlos
        // hacia adelante no rompe SSA dominance: si un CONST se usaba a
        // X, ahora esta definido aun antes que X.
        {
            std::vector<IrInstr> phis, consts, rest;
            phis.reserve(4); consts.reserve(8); rest.reserve(B.instrs.size());
            for (auto &ins : B.instrs) {
                if (ins.op == IrOp::PHI)        phis.push_back(std::move(ins));
                else if (ins.op == IrOp::CONST) consts.push_back(std::move(ins));
                else                            rest.push_back(std::move(ins));
            }
            B.instrs.clear();
            B.instrs.reserve(phis.size() + consts.size() + rest.size());
            for (auto &ins : phis)   B.instrs.push_back(std::move(ins));
            for (auto &ins : consts) B.instrs.push_back(std::move(ins));
            for (auto &ins : rest)   B.instrs.push_back(std::move(ins));
        }

        auto rewrite_phi_block_ref = [&](IrBlockId target_id) {
            if (target_id >= fn.blocks.size()) return;
            IrBlock &T = fn.blocks[target_id];
            for (auto &ins : T.instrs) {
                if (ins.op != IrOp::PHI) break;
                for (auto &pa : ins.phi_args) {
                    if (pa.block == hid) pa.block = static_cast<IrBlockId>(bi);
                }
            }
        };
        rewrite_phi_block_ref(t_true);
        rewrite_phi_block_ref(t_false);

        changed = true;
        // No avanzar bi: re-procesar B porque puede haber cadena
        // (B -> H1 -> H2 inlinable transitivamente).
        --bi;
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_inline
// =========================================================================
//
// Function inlining a nivel modulo.  Para cada CALL site, si el callee
// cumple las heuristicas de inlineabilidad, sustituye la CALL con el
// cuerpo del callee, renombrando SSA values + bloques.
//
// = v1: Single-block callees =
//
// El callee debe tener exactamente 1 bloque que termine con RET (o no
// terminar si vacio).  Esto cubre casos muy comunes:
//   - Wrappers triviales: `i32 f() { return 0; }`
//   - Getters: `i32 get_x(T this) { return this.x; }`
//   - Helpers pequenos sin control de flujo.
//
// Multi-block callees y recursion requieren CFG manipulation mas
// compleja (CFG merge + phi insertion) -- deferred a v2.
//
// = Heuristica =
//
// Inlineable si:
//   - Callee esta definido en nuestro IrModule (no @c is_native).
//   - Callee tiene 1 bloque exactamente y termina con RET.
//   - Body del callee tiene < INLINE_THRESHOLD instrucciones.
//   - Callee NO es el caller (no self-inline).
//
// = Beneficios =
//
// JIT: skip CALL/RET overhead, regalloc puede ver mas contexto,
// const-fold se propaga a traves de la call.  Ejemplo: pruebas() en
// 100_reflection_full despues de dead-alloc elim queda como
// `return 0`.  Si se inline en main, la asignacion a val es trivial
// y el loop body se reduce a la comparacion + incremento.
//
// port-C: codigo destino mas legible, sin auxiliary functions ni
// goto-style returns.

bool ir_pass_inline(IrModule &mod) {
    /* Threshold de tamano del body del callee para inlinar.
     *
     * Por que 12 (en lugar de 8 o 16): el overhead del CALLVM (push regs vivos,
     * mov r1..rN args, callvm, pop regs, mov dst r0) en el peor caso son ~24
     * instrucciones VM.  Cualquier callee cuyo cuerpo cabe en menos de eso es
     * candidato directo a inline ya que ahorramos mas de lo que crece el
     * caller.  12 es el balance que captura getters, setters y helpers
     * aritmeticos pequenos (ej. computos como ladominos lados, 2 LOADs + MUL
     * + DIV + RET) sin causar bloat material en el .velb de programas
     * tipicos.  Subir el limite por encima de ~16 empieza a inflar el binario
     * sin ganancia neta porque los callees grandes ya son rentables como
     * subrutina compartida. */
    constexpr size_t INLINE_THRESHOLD = 12;
    bool changed = false;

    /* Build name -> index map. */
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        name_to_idx[mod.functions[i].name] = i;
    }

    /* Funciones que NUNCA se deben inlinear (entry points alternativos
     * o sintetic functions con calling conventions especiales). */
    auto is_blacklisted = [](const std::string &name) -> bool {
        /* __module_init: el Loader lo invoca via init_pc, no es un CALL
         * normal.  Inlinearlo en main duplica el defclass + deffield
         * + defmethod del bytecode. */
        if (name == "__module_init") return true;
        /* Lambda helpers: invocados via function pointer en CALLCLOSURE.
         * Sus IR son single-block + RET pero el calling convention es
         * distinta (env_addr en r14, etc). */
        if (name.size() > 9 && name.compare(0, 9, "__lambda_") == 0) return true;
        /* Spawn helpers: invocados por SPAWN op, no por CALL. */
        if (name.size() > 8 && name.compare(0, 8, "__spawn_") == 0) return true;
        /* Async helpers: invocados por @Async machinery. */
        if (name.size() > 8 && name.compare(0, 8, "__async_") == 0) return true;
        /* rspawn body helpers. */
        if (name.size() > 9 && name.compare(0, 9, "__rspawn_") == 0) return true;
        return false;
    };

    /* Pre-classify cada function: es inlineable? */
    auto is_inlineable = [&](const IrFunction &fn) -> bool {
        if (fn.is_native) return false;
        if (is_blacklisted(fn.name)) return false;
        if (fn.blocks.size() != 1) return false;
        if (fn.blocks[0].instrs.empty()) return false;
        /* Ultima instr debe ser RET. */
        const auto &last = fn.blocks[0].instrs.back();
        if (last.op != IrOp::RET) return false;
        if (fn.blocks[0].instrs.size() > INLINE_THRESHOLD) return false;
        /* No inlinear funciones que contengan CALLs recursivas a si mismas. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if ((ins.op == IrOp::CALL || ins.op == IrOp::TAILCALL)
             && ins.func_name == fn.name) {
                return false;
            }
        }
        /* No inlinear funciones que tengan @c RAW_ASM en su body cuando
         * el RAW_ASM podria depender del calling convention especifico
         * de la callee.  Conservadoramente: skip si hay raw_asm. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if (ins.op == IrOp::RAW_ASM) return false;
        }
        return true;
    };

    /* Cache de classification. */
    std::vector<bool> can_inline(mod.functions.size(), false);
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        can_inline[i] = is_inlineable(mod.functions[i]);
    }

    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        IrFunction &caller = mod.functions[fi];
        if (caller.is_native) continue;

        for (auto &bb : caller.blocks) {
            /* Procesar in-place; recolectar lista de cambios primero
             * para no invalidar iterators. */
            std::vector<IrInstr> new_instrs;
            new_instrs.reserve(bb.instrs.size());

            for (size_t i = 0; i < bb.instrs.size(); ++i) {
                IrInstr &ins = bb.instrs[i];
                if (ins.op != IrOp::CALL) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                /* CALL a function user.  Verificar si el callee esta
                 * en el modulo y es inlineable. */
                auto it = name_to_idx.find(ins.func_name);
                if (it == name_to_idx.end() || it->second == fi
                 || !can_inline[it->second]) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                const IrFunction &callee = mod.functions[it->second];
                const IrBlock &cbody = callee.blocks[0];
                /* Aridad must match: params.size() == operands.size(). */
                if (callee.params.size() != ins.operands.size()) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }

                /* Mapeo callee_vid -> caller_vid. */
                std::unordered_map<IrValueId, IrValueId> vmap;
                /* Params del callee se mapean a operandos del CALL. */
                for (size_t pi = 0; pi < callee.params.size(); ++pi) {
                    vmap[callee.params[pi]] = ins.operands[pi];
                }

                /* Helper: para cada SSA value que el callee DEFINE,
                 * allocar fresh en el caller. */
                auto remap_dst = [&](IrValueId cvid, IrType type,
                                     const std::string &name_hint) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    const IrValueId new_vid = static_cast<IrValueId>(caller.values.size());
                    IrValue nv{};
                    nv.id   = new_vid;
                    nv.type = type;
                    nv.name = "%inl_" + std::to_string(new_vid);
                    (void)name_hint;
                    if (cvid < callee.values.size()) {
                        const auto &cv = callee.values[cvid];
                        nv.is_const          = cv.is_const;
                        nv.const_val         = cv.const_val;
                        nv.is_host_ptr       = cv.is_host_ptr;
                        nv.pointee_is_host_ptr = cv.pointee_is_host_ptr;
                        nv.is_gc_object      = cv.is_gc_object;
                        nv.narrow_only       = cv.narrow_only;
                    }
                    caller.values.push_back(nv);
                    vmap[cvid] = new_vid;
                    return new_vid;
                };

                auto remap_op = [&](IrValueId cvid) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    /* Valor del callee que no fue param ni dst previo.
                     * Esto no deberia pasar si procesamos en orden. */
                    return IR_NO_VALUE;
                };

                /* Replicar todas las instrs del callee EXCEPTO el RET final. */
                IrValueId ret_value = IR_NO_VALUE;
                bool inline_ok = true;
                for (const auto &c_ins : cbody.instrs) {
                    if (c_ins.op == IrOp::RET) {
                        if (!c_ins.operands.empty()) {
                            ret_value = remap_op(c_ins.operands[0]);
                        }
                        continue;  /* skip RET; ret value resolved */
                    }
                    /* Clonar c_ins y remap operandos + dst. */
                    IrInstr ni = c_ins;
                    /* dst: si tiene resultado, mapear a nuevo VID en caller. */
                    if (ni.dst != IR_NO_VALUE) {
                        const IrType dst_type = (ni.dst < callee.values.size())
                            ? callee.values[ni.dst].type : ni.type;
                        ni.dst = remap_dst(ni.dst, dst_type, "");
                    }
                    /* operands. */
                    for (auto &op : ni.operands) {
                        op = remap_op(op);
                    }
                    /* func_ptr (CALLIND/CALLCLOSURE). */
                    if (ni.func_ptr != IR_NO_VALUE) {
                        ni.func_ptr = remap_op(ni.func_ptr);
                    }
                    /* phi_args: callee es single-block, no phis razonables.
                     * Si los hay, skip inline. */
                    if (!ni.phi_args.empty()) {
                        inline_ok = false;
                        break;
                    }
                    new_instrs.push_back(std::move(ni));
                }

                if (!inline_ok) {
                    /* Cancelar el inline: revertir lo que anyadimos.
                     * Conservativo: push el CALL original. */
                    /* Quitar las instrs recien anyadidas relacionadas con inline.
                     * Para simplicidad: NO retroceder; quedaria un mix
                     * incorrecto.  Marcar y emit CALL original al final. */
                    /* Reset estrategia: para evitar IR corrupto, hacemos un
                     * passthrough simple: no haber comenzado a anyadir.
                     * Como ya empezamos, no podemos limpiar facilmente.
                     * Por seguridad: rebuild new_instrs desde scratch
                     * usando bb.instrs[0..i]. */
                    new_instrs.clear();
                    for (size_t k = 0; k <= i; ++k) {
                        new_instrs.push_back(bb.instrs[k]);
                    }
                    continue;
                }

                /* Emitir el "resultado" del inline: si el CALL tenia dst,
                 * MOV dst <- ret_value. */
                if (ins.dst != IR_NO_VALUE && ret_value != IR_NO_VALUE) {
                    IrInstr mv{};
                    mv.op = IrOp::MOV;
                    mv.type = ins.type;
                    mv.dst = ins.dst;
                    mv.operands = {ret_value};
                    new_instrs.push_back(std::move(mv));
                } else if (ins.dst != IR_NO_VALUE) {
                    /* Callee no retorno valor pero CALL expected uno.
                     * Conservativo: CONST 0 placeholder. */
                    IrInstr cz{};
                    cz.op = IrOp::CONST;
                    cz.type = ins.type;
                    cz.dst = ins.dst;
                    cz.imm = 0;
                    new_instrs.push_back(std::move(cz));
                }

                changed = true;
            }
            bb.instrs = std::move(new_instrs);
        }
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_licm
// =========================================================================
//
// Loop-Invariant Code Motion.  Para cada loop simple detectado, mueve
// instrucciones cuyos operandos son TODOS invariantes (definidos fuera
// del loop O constantes) al predecesor del loop header.
//
// Beneficio: las invariantes se calculan UNA vez (en el predecesor) en
// vez de N veces (en cada iteracion del loop).  Tipico para patrones
// como `for(i; i<size; i++)` donde `size` es invariant.
//
// = v1: simple algorithm =
//
// 1. Detectar loops via back-edges (JMP/BR_COND a un bloque ANTERIOR
//    en orden lineal o via dataflow simple).
// 2. Para cada loop header H y back-edge from B:
//    - Conjunto de bloques en el loop: { H, ..., B } (BFS desde B
//      hasta H siguiendo predecesores).
//    - Pre-header: bloque que cae a H pero NO esta en el loop.
//      Si hay multiples, abortar (necesita split, deferred).
// 3. Para cada instr en el loop:
//    - Si pure_op AND todos los operands son CONSTs o definidos fuera
//      del loop AND la instr no es phi AND no tiene side effects:
//      Mover al pre-header.
//
// v1 conservativo: solo loops simples con UN solo back-edge y UN pre-
// header (case clasico while/for).

bool ir_pass_licm(IrFunction &fn) {
    if (fn.blocks.size() < 3) return false;  /* necesita pre-header + body + header */
    const size_t N = fn.blocks.size();

    /* Construir CFG: para cada bloque, sus sucesores y predecesores. */
    std::vector<std::vector<IrBlockId>> preds(N);
    std::vector<std::vector<IrBlockId>> succs(N);
    for (size_t b = 0; b < N; ++b) {
        const auto &bb = fn.blocks[b];
        if (bb.instrs.empty()) continue;
        const auto &last = bb.instrs.back();
        IrBlockId t1 = IR_NO_BLOCK, t2 = IR_NO_BLOCK;
        if (last.op == IrOp::BR) {
            t1 = last.target_block;
        } else if (last.op == IrOp::BR_COND) {
            t1 = last.target_block;
            t2 = last.false_block;
        }
        if (t1 != IR_NO_BLOCK && t1 < N) {
            preds[t1].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t1);
        }
        if (t2 != IR_NO_BLOCK && t2 < N) {
            preds[t2].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t2);
        }
    }

    /* Dominadores via Cooper-Harvey-Kennedy iterativo.
     * dom[entry] = entry, otros = UNDEF.  Procesar en reverse postorder
     * hasta punto fijo.  intersect(b1, b2) sube en el dom-tree hasta
     * encontrar el ancestro comun mas cercano. */
    const IrBlockId UNDEF = static_cast<IrBlockId>(N);
    const IrBlockId entry = 0;  /* convencion: bloque 0 es entry */

    /* DFS para reverse postorder. */
    std::vector<IrBlockId> rpo;
    rpo.reserve(N);
    {
        std::vector<bool> visited(N, false);
        std::function<void(IrBlockId)> dfs = [&](IrBlockId b) {
            if (b >= N || visited[b]) return;
            visited[b] = true;
            for (IrBlockId s : succs[b]) dfs(s);
            rpo.push_back(b);
        };
        dfs(entry);
        std::reverse(rpo.begin(), rpo.end());
    }
    /* rpo_pos[b] = posicion de b en rpo (mayor = mas adelante = mas alto).
     * Usado por intersect_dom.  Bloques no alcanzables tienen UNDEF rpo_pos. */
    std::vector<uint32_t> rpo_pos(N, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = static_cast<uint32_t>(i);

    /* idom[b] = inmediato dominador.  UNDEF = no computado todavia. */
    std::vector<IrBlockId> idom(N, UNDEF);
    idom[entry] = entry;

    auto intersect_dom = [&](IrBlockId b1, IrBlockId b2) -> IrBlockId {
        while (b1 != b2) {
            while (b1 != UNDEF && rpo_pos[b1] > rpo_pos[b2]) b1 = idom[b1];
            while (b2 != UNDEF && rpo_pos[b2] > rpo_pos[b1]) b2 = idom[b2];
            if (b1 == UNDEF || b2 == UNDEF) return UNDEF;
        }
        return b1;
    };

    bool dom_changed = true;
    while (dom_changed) {
        dom_changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = UNDEF;
            for (IrBlockId p : preds[b]) {
                if (idom[p] != UNDEF) {
                    new_idom = (new_idom == UNDEF) ? p : intersect_dom(new_idom, p);
                    if (new_idom == UNDEF) break;
                }
            }
            if (new_idom != UNDEF && new_idom != idom[b]) {
                idom[b] = new_idom;
                dom_changed = true;
            }
        }
    }

    /* Helper: T domina B?  Camina la cadena idom desde B hasta entry o T. */
    auto dominates = [&](IrBlockId T, IrBlockId B) -> bool {
        if (T == B) return true;
        if (T >= N || B >= N || idom[B] == UNDEF) return false;
        IrBlockId cur = B;
        while (idom[cur] != cur) {
            cur = idom[cur];
            if (cur == T) return true;
        }
        return false;
    };

    /* Back-edge real: arista B->T donde T domina a B. */
    struct BackEdge { IrBlockId pred; IrBlockId header; };
    std::vector<BackEdge> backs;
    for (size_t b = 0; b < N; ++b) {
        for (IrBlockId s : succs[b]) {
            if (dominates(s, static_cast<IrBlockId>(b))) {
                backs.push_back({static_cast<IrBlockId>(b), s});
            }
        }
    }

    if (backs.empty()) return false;

    bool changed = false;

    /* Procesar cada back-edge (= 1 loop). */
    for (const auto &be : backs) {
        const IrBlockId header = be.header;
        const IrBlockId back   = be.pred;

        /* Bloques en el loop: BFS reverse desde back hasta header via
         * preds (en CFG reducible). */
        std::unordered_set<IrBlockId> loop_set;
        loop_set.insert(header);
        loop_set.insert(back);
        std::vector<IrBlockId> stack{back};
        while (!stack.empty()) {
            IrBlockId b = stack.back();
            stack.pop_back();
            if (b == header) continue;
            for (IrBlockId p : preds[b]) {
                if (!loop_set.count(p)) {
                    loop_set.insert(p);
                    stack.push_back(p);
                }
            }
        }

        /* Pre-header: predecesor de @c header que NO esta en el loop.
         * Tipicamente entry o un bloque previo al while. */
        IrBlockId pre_header = IR_NO_BLOCK;
        for (IrBlockId p : preds[header]) {
            if (!loop_set.count(p)) {
                if (pre_header == IR_NO_BLOCK) {
                    pre_header = p;
                } else {
                    /* Multiples pre-headers: skip (necesita split). */
                    pre_header = IR_NO_BLOCK;
                    break;
                }
            }
        }
        if (pre_header == IR_NO_BLOCK) continue;

        /* Conjunto de VIDs definidos DENTRO del loop. */
        std::unordered_set<IrValueId> defined_in_loop;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                if (ins.dst != IR_NO_VALUE) {
                    defined_in_loop.insert(ins.dst);
                }
            }
        }

        /* Detectar si hay STORE/MEMCPY/SETFIELD/ARRAY_STORE/RAW_ASM/CALL
         * dentro del loop.  Si los hay, los LOADs no son hoistables sin
         * alias analysis (la memoria pudo cambiar entre iteraciones). */
        bool loop_has_memory_writes = false;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                switch (ins.op) {
                    case IrOp::STORE: case IrOp::MEMCPY: case IrOp::SETFIELD:
                    case IrOp::ARRAY_STORE: case IrOp::STRFINALIZE:
                    case IrOp::RAW_ASM:
                    case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
                    case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
                    case IrOp::TAILCALL:
                        loop_has_memory_writes = true;
                        break;
                    default: break;
                }
                if (loop_has_memory_writes) break;
            }
            if (loop_has_memory_writes) break;
        }

        /* Helper: instr es candidato para mover? */
        auto is_invariant_candidate = [&](const IrInstr &ins) -> bool {
            if (!is_pure(ins.op)) return false;
            if (ins.op == IrOp::PHI) return false;
            if (ins.preserve) return false;
            if (ins.dst == IR_NO_VALUE) return false;
            /* Ops que LEEN memoria: solo hoistables si NO hay writes en el
             * loop (v1 sin alias analysis).  Conservador pero correcto. */
            switch (ins.op) {
                case IrOp::LOAD: case IrOp::ARRAY_LOAD: case IrOp::GETFIELD:
                case IrOp::ARRAY_LEN:
                    if (loop_has_memory_writes) return false;
                    break;
                default: break;
            }
            /* Todos los operands deben ser invariant (CONST o definidos fuera). */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                if (defined_in_loop.count(op)) return false;
            }
            return true;
        };

        /* Recolectar instrucciones a mover (en orden) + remover de sus
         * bloques originales. */
        std::vector<IrInstr> moved;
        for (IrBlockId b : loop_set) {
            auto &bb = fn.blocks[b];
            std::vector<IrInstr> remaining;
            remaining.reserve(bb.instrs.size());
            for (auto &ins : bb.instrs) {
                if (is_invariant_candidate(ins)) {
                    /* Mover.  Despues de mover, el VID ya no esta
                     * definido en el loop -> otros usos del mismo
                     * VID dentro del loop necesitan que la def este
                     * en pre-header (ok, sera invariant alli). */
                    moved.push_back(std::move(ins));
                    defined_in_loop.erase(moved.back().dst);
                    changed = true;
                } else {
                    remaining.push_back(std::move(ins));
                }
            }
            bb.instrs = std::move(remaining);
        }

        if (moved.empty()) continue;

        /* Insertar las moved instrs al final del pre-header, ANTES de
         * su terminador (JMP a header). */
        auto &ph_bb = fn.blocks[pre_header];
        size_t insert_at = ph_bb.instrs.size();
        if (!ph_bb.instrs.empty()) {
            const auto &term = ph_bb.instrs.back();
            if (term.op == IrOp::BR || term.op == IrOp::BR_COND
             || term.op == IrOp::RET || term.op == IrOp::THROW) {
                insert_at = ph_bb.instrs.size() - 1;
            }
        }
        ph_bb.instrs.insert(ph_bb.instrs.begin() + insert_at,
            std::make_move_iterator(moved.begin()),
            std::make_move_iterator(moved.end()));
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_devirt_monomorphic (Phase D.7.opt)
// =========================================================================

/**
 * @brief Detecta si el modulo usa AOP escaneando raw_asm por "addadvice".
 *
 * Conservador: si encuentra cualquier raw_asm con texto "addadvice", el
 * modulo se considera AOP-enabled y se skip-ea el devirt.  Sin esto las
 * callvirt convertidas a call directo saltarian el advice chain runtime.
 */
static bool module_uses_aop(const IrModule &mod) {
    for (const auto &cls : mod.classes) {
        if (cls.is_aspect) return true;
    }
    for (const auto &fn : mod.functions) {
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.op == IrOp::RAW_ASM
                 && ins.func_name.find("addadvice") != std::string::npos) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ir_pass_devirt_monomorphic(IrModule &mod) {
    if (module_uses_aop(mod)) return false;

    /* Indice por nombre de clase. */
    std::unordered_map<std::string, const IrClass *> class_by_name;
    for (const auto &c : mod.classes) class_by_name[c.name] = &c;
    if (class_by_name.empty()) return false;

    /* "Effectively final": clase sin subclases dentro del modulo.
     * Conservador (closed world).  El loadmodule dinamico podria
     * cargar una subclase, pero los tests/e2e no usan ese patron. */
    std::unordered_set<std::string> has_subclass;
    for (const auto &c : mod.classes) {
        if (!c.super_name.empty() && c.super_name != "Object") {
            has_subclass.insert(c.super_name);
        }
    }

    bool changed = false;
    for (auto &fn : mod.functions) {
        if (fn.is_native) continue;
        if (fn.blocks.empty()) continue;

        /* class_of: para cada SSA value cuyo origen es conocido como una
         * clase concreta, mapea vid -> class_name.  Se construye iterando
         * en orden lineal de bloques con propagacion a traves de MOV y
         * PHI (si todos los inputs comparten clase). */
        std::unordered_map<IrValueId, std::string> class_of;

        /* Multiples pasadas para propagar a traves de PHIs (loops). */
        const int MAX_ITERS = 4;
        for (int iter = 0; iter < MAX_ITERS; ++iter) {
            bool grew = false;
            for (auto &bb : fn.blocks) {
                for (auto &ins : bb.instrs) {
                    if (ins.dst == IR_NO_VALUE) continue;
                    if (class_of.count(ins.dst)) continue;

                    /* Origen: call @__new_<X> */
                    if (ins.op == IrOp::CALL
                     && ins.func_name.rfind("__new_", 0) == 0) {
                        std::string cn = ins.func_name.substr(6);
                        if (class_by_name.count(cn)) {
                            class_of[ins.dst] = cn;
                            grew = true;
                        }
                        continue;
                    }
                    /* NEWOBJ no carga class_name directamente; ignorar. */
                    /* MOV: hereda clase del source */
                    if (ins.op == IrOp::MOV && !ins.operands.empty()) {
                        auto it = class_of.find(ins.operands[0]);
                        if (it != class_of.end()) {
                            class_of[ins.dst] = it->second;
                            grew = true;
                        }
                        continue;
                    }
                    /* PHI: si TODOS los inputs comparten clase, hereda. */
                    if (ins.op == IrOp::PHI && !ins.phi_args.empty()) {
                        std::string c;
                        bool ok = true;
                        for (const auto &pa : ins.phi_args) {
                            if (pa.value == IR_NO_VALUE
                             || pa.value == ins.dst) continue;
                            auto it = class_of.find(pa.value);
                            if (it == class_of.end()) { ok = false; break; }
                            if (c.empty()) c = it->second;
                            else if (c != it->second) { ok = false; break; }
                        }
                        if (ok && !c.empty()) {
                            class_of[ins.dst] = c;
                            grew = true;
                        }
                        continue;
                    }
                }
            }
            if (!grew) break;
        }

        /* Aplicar devirt. */
        for (auto &bb : fn.blocks) {
            for (auto &ins : bb.instrs) {
                if (ins.op != IrOp::CALLVIRT) continue;
                if (ins.operands.empty()) continue;
                auto it = class_of.find(ins.operands[0]);
                if (it == class_of.end()) continue;
                const std::string &cn = it->second;
                auto ct = class_by_name.find(cn);
                if (ct == class_by_name.end()) continue;
                const IrClass *cls = ct->second;
                /* Safe: clase final O sin subclases EN ESTE MODULO. */
                const bool safe_class = cls->is_final
                                     || !has_subclass.count(cn);
                /* Buscar el metodo en el vtable_index. */
                const IrMethod *mtd = nullptr;
                for (const auto &m : cls->methods) {
                    if (m.vtable_index == static_cast<int32_t>(ins.imm)) {
                        mtd = &m;
                        break;
                    }
                }
                if (!mtd) continue;
                if (mtd->ir_fn_name.empty()) continue;
                const bool safe_method = mtd->is_final || safe_class;
                if (!safe_method) continue;

                /* Rewrite CALLVIRT -> CALL */
                ins.op        = IrOp::CALL;
                ins.func_name = mtd->ir_fn_name;
                ins.imm       = 0;
                /* operands sin cambio: [obj, args...] */
                changed = true;
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
// =========================================================================

bool ir_pass_load_narrow(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    /* Ops "narrow-safe": dado inputs con bits bajos correctos (sin importar
     * bits altos), producen un resultado cuyos bits bajos siguen siendo
     * correctos.  ADD/SUB/MUL/AND/OR/XOR son bit-parallel en los bits bajos. */
    auto is_narrow_safe_arith = [](IrOp op) -> bool {
        switch (op) {
            case IrOp::ADD:
            case IrOp::SUB:
            case IrOp::MUL:
            case IrOp::AND:
            case IrOp::OR:
            case IrOp::XOR:
                return true;
            default:
                return false;
        }
    };

    /* Construir lista de usos (vid -> [(block_idx, instr_idx, kind)]).
     * kind: 0=operands, 1=phi_args.  Solo necesitamos saber QUE instrucciones
     * referencian cada valor para inspeccionar su op. */
    struct UseRef {
        size_t  bi;
        size_t  ii;
    };
    std::unordered_map<IrValueId, std::vector<UseRef>> uses;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
            const auto &ins = bb.instrs[ii];
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) uses[op].push_back({bi, ii});
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) uses[pa.value].push_back({bi, ii});
            }
        }
    }

    /* Para cada LOAD i8/i16/i32 (signed), computar el cierre transitivo
     * de valores derivados via ops narrow-safe.  Si TODOS los usos terminales
     * son STORE/RET del mismo tipo y todos los usos intermedios son ops
     * narrow-safe o STORE/RET, marcar el LOAD como narrow_only. */
    /* Re-analizar en cada invocacion: el flag puede REVOCARSE si pasos
     * posteriores (CSE/copy_prop) exponen usos que antes no eran visibles
     * (e.g. %42 = add %38, %41 cuya operand %41 luego se rewrite a %29). */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.op != IrOp::LOAD) continue;
            if (ins.dst == IR_NO_VALUE) continue;
            if (ins.type != IrType::I8 && ins.type != IrType::I16
             && ins.type != IrType::I32) continue;

            const IrType narrow_type = ins.type;

            /* Cierre transitivo via BFS. */
            std::unordered_set<IrValueId> closure;
            std::vector<IrValueId> worklist;
            closure.insert(ins.dst);
            worklist.push_back(ins.dst);

            bool safe = true;
            while (safe && !worklist.empty()) {
                IrValueId v = worklist.back();
                worklist.pop_back();

                auto it = uses.find(v);
                if (it == uses.end()) continue; // no uses -> trivially safe

                for (const UseRef &u : it->second) {
                    const IrInstr &user = fn.blocks[u.bi].instrs[u.ii];

                    /* STORE del mismo tipo: el valor solo se usa como
                     * operand[0] (val).  Truncar al ancho de tipo es seguro. */
                    if (user.op == IrOp::STORE) {
                        if (user.type != narrow_type) { safe = false; break; }
                        /* El valor solo es seguro si esta en operand[0] (val);
                         * si esta en operand[1] (ptr), eso seria un puntero
                         * derivado del LOAD lo cual es UNSAFE (no es nuestro
                         * caso esperado pero por seguridad). */
                        if (user.operands.size() < 2) { safe = false; break; }
                        if (user.operands[0] != v) { safe = false; break; }
                        continue;
                    }

                    /* RET del mismo tipo: el caller espera el ancho declarado,
                     * el VM trunca al hacer return.  Conservadoramente solo
                     * permitimos cuando fn.ret_type coincide. */
                    if (user.op == IrOp::RET) {
                        if (fn.ret_type != narrow_type) { safe = false; break; }
                        continue;
                    }

                    /* Op narrow-safe del mismo tipo: propagar al closure. */
                    if (is_narrow_safe_arith(user.op) && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* PHI del mismo tipo: el PHI mismo produce un valor i32
                     * cuyos bits altos pueden ser garbage si NUESTRO valor
                     * llega.  Pero si TODOS los usos del PHI son seguros, el
                     * garbage no importa.  Propagar el dst del PHI al closure
                     * para que la BFS verifique sus usos transitivamente.
                     * Los ciclos en PHIs de loops se manejan via el set
                     * @c closure (no se re-procesa lo ya visitado).
                     * Otros inputs del PHI no nos importan: solo nos
                     * preocupa como nuestro valor se propaga a partir del
                     * PHI hacia adelante. */
                    if (user.op == IrOp::PHI && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* MOV del mismo tipo (e.g. copy_prop residual): propagar. */
                    if (user.op == IrOp::MOV && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* Cualquier otro uso (CMP, SEXT, ZEXT, CAST, BITCAST,
                     * TRUNC, SHL/SHR/SAR, NEG/NOT, SDIV/UDIV/SMOD/UMOD,
                     * CALL, STORE/LOAD/RET de tipo distinto, etc.) aborta
                     * la elision -- los bits altos pueden ser necesarios. */
                    safe = false;
                    break;
                }
            }

            /* Establecer/revocar el flag segun analisis actual. */
            const bool prev = fn.values[ins.dst].narrow_only;
            if (prev != safe) {
                fn.values[ins.dst].narrow_only = safe;
                changed = true;
            }
        }
    }

    return changed;
}

// =========================================================================
//  Pase List Scheduling: reordena para exponer ILP
// =========================================================================

/* Determina si la instruccion es una "barrera" (no se puede reordenar
 * a traves de ella en NINGUNA direccion: ni mover instrucciones hacia
 * arriba de la barrera, ni hacia abajo).  Usado para CALLs, RAW_ASM,
 * NEWOBJ, etc. donde el side-effect es opaco al scheduler. */
static bool is_sched_barrier(IrOp op) {
    switch (op) {
        case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
        case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLCLOSURE:
        case IrOp::TAILCALL:
        case IrOp::RAW_ASM:
        case IrOp::NEWOBJ: case IrOp::GC_ALLOC:
        case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
        case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
        case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
        case IrOp::MEMCPY:
        case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
        case IrOp::FUTURE: case IrOp::AWAIT: case IrOp::FULFILL: case IrOp::REJECT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:
            return true;
        default:
            return false;
    }
}

/* STORE no es barrera total pero sirve de "memory barrier" suave:
 * LOADs posteriores podrian alias, asi que LOAD depende de todos los
 * STOREs previos del mismo bloque (conservativo).  Otros STOREs tambien
 * dependen del previo (orden de escritura es observable). */
static bool is_store_like(IrOp op) {
    return op == IrOp::STORE || op == IrOp::SETFIELD
        || op == IrOp::ARRAY_STORE || op == IrOp::MEMCPY;
}

static bool is_load_like(IrOp op) {
    return op == IrOp::LOAD || op == IrOp::GETFIELD
        || op == IrOp::ARRAY_LOAD || op == IrOp::ARRAY_LEN;
}

/* Terminadores: deben quedar al final del bloque. */
static bool is_sched_terminator(IrOp op) {
    return op == IrOp::BR || op == IrOp::BR_COND
        || op == IrOp::RET || op == IrOp::THROW;
}

bool ir_pass_schedule(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        const size_t N = bb.instrs.size();
        if (N <= 2) continue; // nada que reordenar

        /* Identificar prefijo de PHIs (fijo al inicio) y terminador. */
        size_t first_movable = 0;
        while (first_movable < N && bb.instrs[first_movable].op == IrOp::PHI) {
            ++first_movable;
        }
        size_t last_movable = N;
        if (last_movable > 0 && is_sched_terminator(bb.instrs[last_movable - 1].op)) {
            --last_movable;
        }
        if (last_movable - first_movable < 2) continue; // <2 instrucciones movibles

        const size_t M = last_movable - first_movable;

        /* Construir DAG de dependencias.  Nodos = indices [0..M) en el
         * rango movible.  Edges: pred[i] = lista de nodos que i depende.
         * succ[i] = lista de nodos que dependen de i. */
        std::vector<std::vector<size_t>> preds(M);
        std::vector<std::vector<size_t>> succs(M);
        std::vector<size_t> in_degree(M, 0);

        /* Map: def_vid -> index dentro de [0..M) que lo define. */
        std::unordered_map<IrValueId, size_t> def_of;
        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];
            if (ins.dst != IR_NO_VALUE) def_of[ins.dst] = i;
        }

        /* Tracking de "ultima barrera/store/load" para deps de memoria. */
        long last_barrier = -1;
        long last_store   = -1;
        std::vector<size_t> loads_after_last_store; // LOADs posteriores al ultimo store

        auto add_edge = [&](size_t from, size_t to) {
            /* Evitar duplicados.  Sanity: from != to. */
            if (from == to) return;
            for (size_t p : preds[to]) if (p == from) return;
            preds[to].push_back(from);
            succs[from].push_back(to);
        };

        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];

            /* Data deps: para cada operando con def en este bloque, edge def->i. */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                auto it = def_of.find(op);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                auto it = def_of.find(pa.value);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = def_of.find(ins.func_ptr);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }

            /* Memory/side-effect deps. */
            const bool is_barr = is_sched_barrier(ins.op);
            const bool is_st   = is_store_like(ins.op);
            const bool is_ld   = is_load_like(ins.op);

            if (is_barr) {
                /* Barrera: depende de todo lo previo, bloquea todo lo posterior.
                 * Conservador: anyadir edge desde TODOS los nodos previos. */
                for (size_t j = 0; j < i; ++j) add_edge(j, i);
                last_barrier = static_cast<long>(i);
                last_store   = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_st) {
                /* STORE depende de la ultima barrera, del ultimo store, y de
                 * todos los LOADs posteriores al ultimo store (orden W-after-R). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                for (size_t ld_idx : loads_after_last_store) add_edge(ld_idx, i);
                last_store = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_ld) {
                /* LOAD depende de la ultima barrera y del ultimo store. */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                loads_after_last_store.push_back(i);
            } else {
                /* Pure ops: data deps + dep contra ultima barrera para
                 * evitar hoist sobre CALL/etc.  Sin esto, un @c const.i32
                 * declarado DESPUES de un CALL podria moverse ANTES del
                 * CALL, aumentando register pressure (el const queda vivo
                 * across el call y debe salvarse en push/pop).  El bench
                 * fib regresiono al hoistar @c const.i32 2 sobre el call
                 * recursivo.  La barrera actua como "muro de scheduling"
                 * en ambas direcciones (no entran ni salen ops a traves). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
            }
        }

        /* Computar in_degree desde preds. */
        for (size_t i = 0; i < M; ++i) in_degree[i] = preds[i].size();

        /* Computar critical path length (CPL): CPL[i] = 1 + max(CPL[succ]).
         * Calculado en orden topologico inverso (de hojas a raices).
         * Hacemos topo sort primero. */
        std::vector<size_t> topo;
        topo.reserve(M);
        std::vector<size_t> in_deg_copy = in_degree;
        std::vector<size_t> q;
        for (size_t i = 0; i < M; ++i) {
            if (in_deg_copy[i] == 0) q.push_back(i);
        }
        while (!q.empty()) {
            size_t n = q.back(); q.pop_back();
            topo.push_back(n);
            for (size_t s : succs[n]) {
                if (--in_deg_copy[s] == 0) q.push_back(s);
            }
        }
        if (topo.size() != M) continue; // ciclo detectado (no deberia pasar en SSA + DAG)

        std::vector<uint32_t> cpl(M, 0);
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            size_t n = *it;
            uint32_t best = 0;
            for (size_t s : succs[n]) {
                if (cpl[s] > best) best = cpl[s];
            }
            cpl[n] = best + 1;
        }

        /* List scheduling: ready set ordenado por (CPL desc, indice asc para estable). */
        std::vector<size_t> new_order;
        new_order.reserve(M);
        std::vector<size_t> ready;
        ready.reserve(M);
        std::vector<size_t> rem_in = in_degree;
        for (size_t i = 0; i < M; ++i) {
            if (rem_in[i] == 0) ready.push_back(i);
        }

        while (!ready.empty()) {
            /* Elegir el de mayor CPL (criterio clasico de Sethi-Ullman /
             * critical-path scheduling).  Tie-break:
             *   (a) menos sucesores primero -- "leaf" nodes que rellenan
             *       latencia sin extender el chain critico;
             *   (b) indice original menor para estabilidad. */
            size_t best_idx = 0;
            for (size_t j = 1; j < ready.size(); ++j) {
                const size_t a = ready[best_idx];
                const size_t b = ready[j];
                if (cpl[b] > cpl[a]) {
                    best_idx = j;
                } else if (cpl[b] == cpl[a]) {
                    if (succs[b].size() < succs[a].size()) {
                        best_idx = j;
                    } else if (succs[b].size() == succs[a].size() && b < a) {
                        best_idx = j;
                    }
                }
            }
            size_t pick = ready[best_idx];
            ready[best_idx] = ready.back();
            ready.pop_back();
            new_order.push_back(pick);
            for (size_t s : succs[pick]) {
                if (--rem_in[s] == 0) ready.push_back(s);
            }
        }

        if (new_order.size() != M) continue; // sanity

        /* Detectar si el orden cambio.  Si no, skip. */
        bool different = false;
        for (size_t i = 0; i < M; ++i) {
            if (new_order[i] != i) { different = true; break; }
        }
        if (!different) continue;

        /* Aplicar reordenamiento: mover bb.instrs[first_movable + new_order[i]]
         * a posicion first_movable + i.  Hacer una copia temporal porque las
         * indices originales se invalidan al mover. */
        std::vector<IrInstr> reordered;
        reordered.reserve(M);
        for (size_t i = 0; i < M; ++i) {
            reordered.push_back(std::move(bb.instrs[first_movable + new_order[i]]));
        }
        for (size_t i = 0; i < M; ++i) {
            bb.instrs[first_movable + i] = std::move(reordered[i]);
        }
        changed = true;
    }

    return changed;
}

// =========================================================================
//  Punto de entrada principal
// =========================================================================

void ir_optimize(IrModule &mod, OptLevel level) {
    if (level == OptLevel::O0) return; // sin optimizacion

    /* Phase D.7.opt: inline a nivel modulo ANTES del fix-point loop.
     * Despues del inline, los passes per-function se re-aplican sobre
     * el codigo expandido. */
    if (level >= OptLevel::O1) {
        ir_pass_inline(mod);
    }

    // Iterar hasta punto fijo o maximo 8 pasadas
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;

        for (auto &fn : mod.functions) {
            if (fn.is_native) continue; // no optimizar stubs nativos

            // O1: copy + simplify + SR + reassoc + dead-alloc + DCE
            any |= ir_pass_copy_prop(fn);
            any |= ir_pass_simplify(fn);            /* algebraic + cast fold + phi simp */
            any |= ir_pass_strength_reduction(fn);  /* mul/div/mod power-of-2 -> shifts */
            any |= ir_pass_reassoc(fn);             /* (x op c1) op c2 -> x op (c1 op c2) */
            any |= ir_pass_licm(fn);                /* LICM con dominators reales */
            any |= ir_pass_dead_alloc_elim(fn);
            any |= ir_pass_dce(fn);

            if (level >= OptLevel::O2) {
                // O2: plegado de constantes + bloques inalcanzables + TCO.
                any |= ir_pass_const_fold(fn);
                any |= ir_pass_unreachable(fn);
                any |= ir_pass_tailcall(fn);
                // Inline de header trivial de loop -> habilita decjnz fusion.
                any |= ir_pass_inline_loop_header(fn);
                // Dead store elimination: limpia STOREs muertos consecutivos.
                any |= ir_pass_dse(fn);
                // Global const CSE solamente (safer than full CSE).
                // El full CSE local tiene bugs sutiles con LOAD/STORE alias
                // que necesitan alias analysis (deferido a O3+).
                // Global const dedup via DIRECT rewrite (no MOV+copy_prop
                // intermedio).  La version vieja con MOV dejaba is_const
                // stale en fn.values causando fallos no-deterministicos en
                // test 110 (smart pointers SRET).  Esta version sustituye
                // operandos directamente y elimina las CONSTs duplicadas.
                any |= ir_pass_const_cse_entry(fn);
                // CSE local de aritmetica pura (ADD/SUB/MUL/etc.) -- dedupea
                // `add.ptr this, off` triplicados en getters/setters.  Tiene
                // invalidacion correcta para LOAD via side-effects.  Habilita
                // store-to-load forwarding al unificar punteros equivalentes.
                any |= ir_pass_cse(fn);
                // Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
                // cuando todos los usos son arith narrow-safe (ADD/SUB/MUL/
                // AND/OR/XOR) + STORE/RET del mismo ancho.  Ahorra 3 instr VM
                // por LOAD elidido.  Bench struct_field: ~270M instr ahorradas.
                any |= ir_pass_load_narrow(fn);
                // Segunda ronda de DCE tras plegado/TCO/loop header inline/CSE.
                any |= ir_pass_dce(fn);
            }

            if (level >= OptLevel::O3) {
                // O3: pasadas mas costosas (GVN, scheduling, etc.) -- TBD.
            }
        }

        /* Devirt + inline @ O2 al final de cada iteracion del fix-point.
         * Importante hacerlo despues de las per-function passes para que
         * la inline pass vea las callees OPTIMIZADAS (e.g. Counter.inc
         * con 6 instrs en vez de 12), aprobando inline bajo el threshold. */
        if (level >= OptLevel::O2) {
            if (ir_pass_devirt_monomorphic(mod)) any = true;
            if (ir_pass_inline(mod))             any = true;
        }

        if (!any) break; // punto fijo alcanzado
    }

    /* Final pass: list scheduling para ILP.  Una sola pasada despues del
     * fix-point porque el reordenamiento NO produce mas oportunidades de
     * optimizacion (es semanticamente neutro -- mismo DAG, distinto orden).
     * Solo aplica @ O2+ por seguridad. */
    if (level >= OptLevel::O2) {
        for (auto &fn : mod.functions) {
            if (fn.is_native) continue;
            ir_pass_schedule(fn);
        }
    }
}

} // namespace ir
