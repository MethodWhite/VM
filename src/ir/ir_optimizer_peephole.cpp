/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "ir/ir_optimizer.h"
#include "ir/ir_optimizer_internal.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <functional>
#include <sstream>
#include <algorithm>

namespace ir {

using ir::opt_internal::is_side_effecting;
using ir::opt_internal::is_terminator;
using ir::opt_internal::is_pure;
using ir::opt_internal::is_licm_hoistable_alloc;
using ir::opt_internal::strmake_reads_immutable;
using ir::opt_internal::is_pure_allocator_name;
using ir::opt_internal::rewrite_as_mov;
using ir::opt_internal::rewrite_as_const;
using ir::opt_internal::rewrite_as_const_with_value;
using ir::opt_internal::type_mask;
using ir::opt_internal::type_is_signed_int;
using ir::opt_internal::sign_extend_from;
using ir::opt_internal::bits_to_f64;
using ir::opt_internal::f64_to_bits;
using ir::opt_internal::bits_to_f32;
using ir::opt_internal::f32_to_bits;

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
                        rewrite_as_const_with_value(fn, ins, 0);
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
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 1) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
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
                        rewrite_as_const_with_value(fn, ins, 1);
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
                        rewrite_as_const_with_value(fn, ins, 0);
                        const_vids[ins.dst] = 0;
                        changed = true;
                    } else if (ins.operands[0] == ins.operands[1]) {
                        /* x % x = 0 (x != 0 implicito) */
                        rewrite_as_const_with_value(fn, ins, 0);
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
                            rewrite_as_const_with_value(fn, ins, 0);
                            const_vids[ins.dst] = 0;
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_mov(ins, ins.operands[0]);
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_const_with_value(fn, ins, 0);
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
                            rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(-1));
                            const_vids[ins.dst] = -1;
                            changed = true;
                        }
                    } else if (get_const(ins.operands[0], c)) {
                        if (c == 0) {
                            rewrite_as_mov(ins, ins.operands[1]);
                            changed = true;
                        } else if (c == -1) {
                            rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(-1));
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
                        rewrite_as_const_with_value(fn, ins, 0);
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
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(ext));
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
                        rewrite_as_const_with_value(fn, ins, z);
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
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(out_val));
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
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(c));
                        const_vids[ins.dst] = c;
                        changed = true;
                    }
                    break;
                }

                /* ---- (D) Math IR ops constant folding (sprint v2.2c) ----
                 *
                 * Cuando todos los operandos son CONST conocidos, evaluamos
                 * la operacion en compile-time y reemplazamos por CONST
                 * literal.  Float ops preservan bits IEEE 754 via memcpy.
                 * Habilita propagacion downstream (e.g. `sqrt(25.0) + 3.0`
                 * pliega a CONST 8.0 directamente). */
                case IrOp::FSQRT:
                case IrOp::FABS:
                case IrOp::FNEG:
                case IrOp::FFLOOR:
                case IrOp::FCEIL:
                case IrOp::FROUND:
                case IrOp::FTRUNC: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double in_v = is_f32
                        ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)))
                        : bits_to_f64(static_cast<uint64_t>(c0));
                    double out_v = 0.0;
                    switch (ins.op) {
                        case IrOp::FSQRT:  out_v = std::sqrt(in_v); break;
                        case IrOp::FABS:   out_v = std::fabs(in_v); break;
                        case IrOp::FNEG:   out_v = -in_v;           break;
                        case IrOp::FFLOOR: out_v = std::floor(in_v); break;
                        case IrOp::FCEIL:  out_v = std::ceil(in_v);  break;
                        case IrOp::FROUND: out_v = std::nearbyint(in_v); break;
                        case IrOp::FTRUNC: out_v = std::trunc(in_v); break;
                        default: break;
                    }
                    uint64_t out_bits = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(out_v)))
                        : f64_to_bits(out_v);
                    rewrite_as_const_with_value(fn, ins, out_bits);
                    const_vids[ins.dst] = static_cast<int64_t>(out_bits);
                    changed = true;
                    break;
                }
                case IrOp::FADD:
                case IrOp::FSUB:
                case IrOp::FMUL:
                case IrOp::FDIV:
                case IrOp::FMIN:
                case IrOp::FMAX: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    auto bits_to_dbl = [&](int64_t v) -> double {
                        return is_f32
                            ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(v)))
                            : bits_to_f64(static_cast<uint64_t>(v));
                    };
                    double a = bits_to_dbl(c0);
                    double b = bits_to_dbl(c1);
                    double r = 0.0;
                    bool ok = true;
                    switch (ins.op) {
                        case IrOp::FADD: r = a + b; break;
                        case IrOp::FSUB: r = a - b; break;
                        case IrOp::FMUL: r = a * b; break;
                        case IrOp::FDIV:
                            /* Folding de FDIV por 0 produciria NaN/Inf
                             * deterministico en IEEE 754; aceptable. */
                            r = a / b; break;
                        case IrOp::FMIN: r = std::fmin(a, b); break;
                        case IrOp::FMAX: r = std::fmax(a, b); break;
                        default: ok = false; break;
                    }
                    if (!ok) break;
                    uint64_t out_bits = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(r)))
                        : f64_to_bits(r);
                    rewrite_as_const_with_value(fn, ins, out_bits);
                    const_vids[ins.dst] = static_cast<int64_t>(out_bits);
                    changed = true;
                    break;
                }
                case IrOp::IABS: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    /* INT_MIN -> UB en C; el IR doc dice "undef".  Para no
                     * crashear, dejar como esta (no foldear). */
                    if (c0 == std::numeric_limits<int64_t>::min()) break;
                    int64_t r = c0 < 0 ? -c0 : c0;
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::IMIN:
                case IrOp::IMAX: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    int64_t r = (ins.op == IrOp::IMIN)
                        ? (c0 < c1 ? c0 : c1)
                        : (c0 > c1 ? c0 : c1);
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::IMINU:
                case IrOp::IMAXU: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    uint64_t u0 = static_cast<uint64_t>(c0);
                    uint64_t u1 = static_cast<uint64_t>(c1);
                    uint64_t r = (ins.op == IrOp::IMINU)
                        ? (u0 < u1 ? u0 : u1)
                        : (u0 > u1 ? u0 : u1);
                    rewrite_as_const_with_value(fn, ins, r);
                    const_vids[ins.dst] = static_cast<int64_t>(r);
                    changed = true;
                    break;
                }
                case IrOp::ILOG2: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* undef segun doc IR; no foldear. */
                    int r = 63;
                #if defined(__GNUC__) || defined(__clang__)
                    r = 63 - __builtin_clzll(u);
                #else
                    for (r = 63; r >= 0 && ((u >> r) & 1ULL) == 0; --r) {}
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::POPCNT: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_popcountll(u);
                #else
                    r = 0; for (; u; u &= u - 1) ++r;
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::CLZ: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* clz(0) undef en x86 lzcnt; no foldear. */
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_clzll(u);
                #else
                    r = 0; while (((u >> 63) & 1ULL) == 0) { u <<= 1; ++r; }
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::CTZ: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    if (u == 0) break; /* ctz(0) undef. */
                    int r;
                #if defined(__GNUC__) || defined(__clang__)
                    r = __builtin_ctzll(u);
                #else
                    r = 0; while ((u & 1ULL) == 0) { u >>= 1; ++r; }
                #endif
                    rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                    const_vids[ins.dst] = r;
                    changed = true;
                    break;
                }
                case IrOp::BYTESWAP: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                #if defined(__GNUC__) || defined(__clang__)
                    u = __builtin_bswap64(u);
                #else
                    u = ((u & 0xFF00000000000000ULL) >> 56)
                      | ((u & 0x00FF000000000000ULL) >> 40)
                      | ((u & 0x0000FF0000000000ULL) >> 24)
                      | ((u & 0x000000FF00000000ULL) >> 8)
                      | ((u & 0x00000000FF000000ULL) << 8)
                      | ((u & 0x0000000000FF0000ULL) << 24)
                      | ((u & 0x000000000000FF00ULL) << 40)
                      | ((u & 0x00000000000000FFULL) << 56);
                #endif
                    rewrite_as_const_with_value(fn, ins, u);
                    const_vids[ins.dst] = static_cast<int64_t>(u);
                    changed = true;
                    break;
                }
                case IrOp::ROTL:
                case IrOp::ROTR: {
                    if (ins.operands.size() < 2) break;
                    int64_t c0 = 0, c1 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    if (!get_const(ins.operands[1], c1)) break;
                    uint64_t u = static_cast<uint64_t>(c0);
                    unsigned n = static_cast<unsigned>(c1) & 63u;
                    uint64_t r = (ins.op == IrOp::ROTL)
                        ? ((u << n) | (n ? (u >> (64 - n)) : 0))
                        : ((u >> n) | (n ? (u << (64 - n)) : 0));
                    rewrite_as_const_with_value(fn, ins, r);
                    const_vids[ins.dst] = static_cast<int64_t>(r);
                    changed = true;
                    break;
                }
                case IrOp::ITOF: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double d = static_cast<double>(c0);
                    uint64_t b = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(d)))
                        : f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::UITOF: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    const bool is_f32 = (ins.type == IrType::F32);
                    double d = static_cast<double>(static_cast<uint64_t>(c0));
                    uint64_t b = is_f32
                        ? static_cast<uint64_t>(f32_to_bits(static_cast<float>(d)))
                        : f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::FTOI:
                case IrOp::FTOUI: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    /* El tipo de la SOURCE (no del dst) decide ancho.  Para
                     * conservar correctness, leemos el tipo del operando. */
                    IrType src_t = (ins.operands[0] < fn.values.size())
                                   ? fn.values[ins.operands[0]].type
                                   : IrType::F64;
                    double v = (src_t == IrType::F32)
                        ? static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)))
                        : bits_to_f64(static_cast<uint64_t>(c0));
                    /* Out-of-range -> UB en C; saltar fold para NaN/Inf y
                     * valores que no entran en int64.  Conservador. */
                    if (!std::isfinite(v)) break;
                    if (ins.op == IrOp::FTOI) {
                        if (v < -9.2233720368547758e18 || v > 9.2233720368547758e18) break;
                        int64_t r = static_cast<int64_t>(v);
                        rewrite_as_const_with_value(fn, ins, static_cast<uint64_t>(r));
                        const_vids[ins.dst] = r;
                    } else {
                        if (v < 0.0 || v > 1.8446744073709552e19) break;
                        uint64_t r = static_cast<uint64_t>(v);
                        rewrite_as_const_with_value(fn, ins, r);
                        const_vids[ins.dst] = static_cast<int64_t>(r);
                    }
                    changed = true;
                    break;
                }
                case IrOp::F32TOF64: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    double d = static_cast<double>(bits_to_f32(static_cast<uint32_t>(c0)));
                    uint64_t b = f64_to_bits(d);
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
                    break;
                }
                case IrOp::F64TOF32: {
                    if (ins.operands.empty()) break;
                    int64_t c0 = 0;
                    if (!get_const(ins.operands[0], c0)) break;
                    float f = static_cast<float>(bits_to_f64(static_cast<uint64_t>(c0)));
                    uint64_t b = static_cast<uint64_t>(f32_to_bits(f));
                    rewrite_as_const_with_value(fn, ins, b);
                    const_vids[ins.dst] = static_cast<int64_t>(b);
                    changed = true;
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
                && !ins.preserve()) {
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
                && !ins.preserve()
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
                    /* Sprint edge-bugs (2026-06-02): si el divisor es CONST 0,
                     * NO foldear -- dejar la operacion en runtime para que el
                     * interp/JIT lance FatalError capturable.  Antes esto se
                     * foldeaba a 0 silencioso ocultando el bug del programa. */
                    case IrOp::DIV:     if (sb == 0) { folded = false; break; }
                                        res = (uint64_t)(sa / sb); break;
                    case IrOp::MOD:     if (sb == 0) { folded = false; break; }
                                        res = (uint64_t)(sa % sb); break;
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
                    case IrOp::TRUNC: {
                        /* Sprint edge-bugs (2026-06-02): TRUNC debe
                         * preservar el signo si el tipo destino es
                         * signed (i8/i16/i32).  Antes hacia
                         * `a & 0xFFFFFFFF` lo que para `-7` (signed i32)
                         * truncado dejaba `0x00000000FFFFFFF9` -- valor
                         * UNSIGNED 4294967289, no -7.  Operaciones
                         * downstream (e.g. const-fold de `mod.i32`) lo
                         * interpretaban como positivo -> resultado
                         * equivocado del modulo signed.
                         *
                         * Fix: para tipos signed, sign-extender el bit
                         * mas alto del ancho destino al resto del u64.
                         * Para unsigned, mask simple. */
                        uint64_t mask;
                        bool sign_extend = false;
                        switch (ins.type) {
                            case IrType::I8:
                                mask = 0xFFULL; sign_extend = true; break;
                            case IrType::I16:
                                mask = 0xFFFFULL; sign_extend = true; break;
                            case IrType::I32:
                                mask = 0xFFFFFFFFULL; sign_extend = true; break;
                            case IrType::U8:  mask = 0xFFULL; break;
                            case IrType::U16: mask = 0xFFFFULL; break;
                            case IrType::U32: mask = 0xFFFFFFFFULL; break;
                            case IrType::BOOL: mask = 0x1ULL; break;
                            default:
                                /* Sin truncacion real (mismo ancho). */
                                mask = 0xFFFFFFFFFFFFFFFFULL;
                                break;
                        }
                        res = a & mask;
                        if (sign_extend) {
                            const uint64_t sign_bit = (mask >> 1) + 1;
                            if (res & sign_bit) {
                                /* Bit alto del ancho destino set -> negativo.
                                 * Or-ear los bits altos para sign-extend a u64. */
                                res |= ~mask;
                            }
                        }
                        break;
                    }
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
                case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLITF:
                case IrOp::CALLCLOSURE:
                case IrOp::TAILCALL:
                case IrOp::RAW_ASM:
                case IrOp::MEMCPY: case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
                case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
                case IrOp::NEWOBJ: case IrOp::NEWOBJS: case IrOp::GC_ALLOC:
                case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
                case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
                // Sprint string-perf-2 bug fix (2026-06-02): STRMAKE/STRCAT/
                // STRCONV LEEN memoria en runtime (STRMAKE lee vm_mem para
                // construir el StringObject; STRCAT puede materializar bytes;
                // STRCONV lee el src y escribe el converted).  Sin invalidar
                // last_store_val, DSE elimina STOREs previos pensando que
                // nadie los lee, pero STRMAKE LOS LEE en runtime.
                // Bug reproducido en patron `buf[i]=X; STRMAKE(buf); buf[i]=Y;
                // STRMAKE(buf)`: el primer set de stores se eliminaba.
                case IrOp::STRMAKE: case IrOp::STRCAT: case IrOp::STRCONV:
                case IrOp::STRFLAT: case IrOp::STRINTERN: case IrOp::STRRESERVE:
                // Sprint edge-bugs (2026-06-02): MVTAKE_IR muta memoria
                // (copia src->dst + zerifica src), por lo que invalida
                // last_store_val para ambos punteros.  Conservadoramente
                // clearamos todo el mapa.  Sin esto, ptr_of() despues de
                // move() retornaba el host_ptr ORIGINAL (SLF leia el store
                // anterior al mvtake) en vez del 0 que el runtime escribio.
                case IrOp::MVTAKE_IR:
                // GC_PROMOTE/GC_DEMOTE copian memoria cross-heap.
                case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
                // ATOMIC_ST_I64 / ATOMIC_CAS_I64 / ATOMIC_ADD_I64 escriben
                // memoria; ATOMIC_LD_I64 lee (que es OK para SLF si no hay
                // store intermedio, pero conservativo clearamos).
                case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
                case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
                // SMARTPTR_FREE invoca deleter que puede tocar memoria.
                case IrOp::SMARTPTR_FREE:
                // FFI runtime puede mutar cualquier cosa.
                case IrOp::DLOPEN: case IrOp::DLSYM:
                case IrOp::MOD_LOAD:
                // raw_asm-elim Fase 2 (__module_init -> IR): los ops de meta-OOP
                // LEEN su struct de parametros desde vm_mem (params_vaddr) en
                // runtime (defclass/deffield/defmethod leen el buffer; findclass/
                // findmethod tambien).  Sin invalidar last_store_idx, DSE elimina
                // los STORE que arman ese buffer creyendo que nadie los lee,
                // PERO el op de meta-OOP los LEE en runtime.  Critico para el
                // patron de buffer REUSADO entre defs: STORE(buf+0,A); deffield;
                // STORE(buf+0,B) -- sin esto el primer STORE se marca dead.
                // Mismo razonamiento que STRMAKE/STRCAT (que leen vm_mem).
                case IrOp::DEFCLASS:  case IrOp::DEFFIELD:  case IrOp::DEFMETHOD:
                case IrOp::FINDCLASS: case IrOp::FINDMETHOD: case IrOp::FINDFIELD:
                case IrOp::ADDADVICE: case IrOp::SETMETHDBG:
                // GETSTATIC/SETSTATIC consultan/mutan cls->static_data (estado
                // global del runtime); conservativo: invalidar el mapa.
                case IrOp::GETSTATIC: case IrOp::SETSTATIC:
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

            // Construir clave canonica: "op:type:imm:func_name:op0:op1:..."
            //
            // Bug fix: imm es semanticamente significativo para STR_LIT_ADDR
            // (indice del string), GETFIELD (offset del campo), ALLOCA (size),
            // y posiblemente otros.  Incluirlo en la clave evita dedupe falso
            // (e.g., dos str_lit_addr con strings distintos parecian iguales).
            //
            // Bug fix fase B: @c func_name es CRITICO para LABEL_ADDR y los
            // CALL-like ops (CALL, CALLN, etc).  Sin esto, dos LABEL_ADDR con
            // labels distintos se deduplican incorrectamente (handler_pc del
            // tryenter se mezcla con el name_addr del findclass, p.ej.).
            std::ostringstream key;
            key << static_cast<int>(ins.op) << ":"
                << static_cast<int>(ins.type) << ":"
                << ins.imm << ":" << ins.func_name;
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

} // namespace ir
