/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer_internal.h
 * @brief Helpers compartidos de los pases de optimizacion sobre SSA IR.
 *
 * Los helpers que varias pasadas de ir_optimizer usan viven aqui (en un
 * namespace interno) para poder dividir ir_optimizer.cpp en modulos sin
 * duplicarlos ni exportarlos como API publica.
 */

#ifndef VESTA_IR_OPTIMIZER_INTERNAL_H
#define VESTA_IR_OPTIMIZER_INTERNAL_H

#include "ir/ir_optimizer.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace ir {
namespace opt_internal {

    /* Helpers IEEE 754 (preservan bits via memcpy). */
    inline double bits_to_f64(uint64_t b) noexcept {
        double d; std::memcpy(&d, &b, sizeof(d)); return d;
    }
    inline uint64_t f64_to_bits(double d) noexcept {
        uint64_t b; std::memcpy(&b, &d, sizeof(b)); return b;
    }
    inline float bits_to_f32(uint32_t b) noexcept {
        float f; std::memcpy(&f, &b, sizeof(f)); return f;
    }
    inline uint32_t f32_to_bits(float f) noexcept {
        uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b;
    }

    /// True si el op tiene efectos observables (E/S, memoria, call).
    bool is_side_effecting(IrOp op);

    /// True si el op termina un bloque (BR, RET, ...).
    bool is_terminator(IrOp op);

    /// True si el op es puro (sin efectos): !is_side_effecting(op).
    bool is_pure(IrOp op);

    /// True si el op es un ALLOC hoistable por LICM (sin efectos externos).
    bool is_licm_hoistable_alloc(IrOp op);

    /// STRMAKE sobre un literal inmutable no tiene efecto observable.
    bool strmake_reads_immutable(const IrFunction &fn, IrValueId vm_addr);

    /// True si la funcion es un allocator puro (__new_*, etc. sin _shared).
    bool is_pure_allocator_name(const std::string &name);

} // namespace opt_internal
} // namespace ir

#endif // VESTA_IR_OPTIMIZER_INTERNAL_H
