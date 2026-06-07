/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_select.h
 * @brief Instruction selection IR -> MachineIR en forma de registros
 *        VIRTUALES (Phase D.7, commit 4b).  Ver doc/REGALLOC.md.
 *
 * A diferencia del selector v1 (slot-per-value, template load/op/store), este
 * emite operandos VREG directos en forma de TRES operandos
 * (@c "add dst, src1, src2" -> dst = src1 OP src2), MUCHO mas simple: el
 * register allocator + el rewrite (commits 2-4a) se encargan del resto.
 *
 * Mapeo: vreg id == IrValueId (1:1).  La salida pasa por
 * @c build_intervals -> @c linear_scan -> @c rewrite_to_physical -> encoder.
 *
 * Cobertura commit 4b: CONST, ALU entera binaria (ADD/SUB/MUL) y unaria
 * (NEG/NOT), RET; funciones de UN bloque (sin control de flujo todavia).  Si
 * encuentra un op fuera del subset, devuelve @c false y el caller hace
 * fallback (al path de slots o al interprete).  Control de flujo, PHI, calls,
 * floats y la integracion VM_ABI llegan en commits posteriores.
 */

#ifndef VESTA_JIT_VREG_SELECT_H
#define VESTA_JIT_VREG_SELECT_H

#include "jit/machine_ir.h"

#include <functional>
#include <string>

namespace ir { struct IrFunction; }

namespace jit {

    /**
     * @brief Resolver de @c CALL: dado el nombre de la funcion destino,
     *        devuelve su direccion nativa (0 si no resoluble -> fallback).
     */
    using CallResolver = std::function<uint64_t(const std::string &)>;

    /**
     * @struct VregEntries
     * @brief Direcciones de runtime entries que el selector vreg necesita para
     *        bajar ops que llaman al runtime.  0 = no disponible (esos ops caen
     *        a fallback).  Se construye desde @c RuntimeEntries en @c auto_jit.
     */
    struct VregEntries {
        uint64_t callvirt   = 0;  ///< vrt_callvirt(proc, obj, vtbl_idx)
        uint64_t gc_deref   = 0;  ///< vrt_gc_deref(proc, handle)
        uint64_t gc_handle  = 0;  ///< vrt_gc_handle_for_ptr(proc, host_ptr)
        uint64_t raw_alloc  = 0;  ///< vrt_raw_alloc(proc, size)
        uint64_t raw_free   = 0;  ///< vrt_raw_free(proc, host_ptr)
        uint64_t gc_allocp  = 0;  ///< vrt_gc_alloc_payload(proc, size) -> host_ptr
        uint64_t calln      = 0;  ///< vrt_calln(proc, lib_id, fn_id) -- FFI native
    };

    /**
     * @brief Selecciona MachineIR de vregs desde @p fn.
     *
     * @param fn   Funcion IR (subset soportado: ver arriba).
     * @param out  MFunction destino (se sobrescribe).
     * @param ent  Direcciones de runtime entries (callvirt/gc/raw_alloc/calln).
     * @return     @c true si TODOS los ops estan soportados y @p out es
     *             valido; @c false si encuentra un op fuera del subset (en
     *             ese caso @p out queda indefinido y el caller hace fallback).
     */
    bool vreg_select(const ir::IrFunction &fn, MFunction &out,
                     AbiKind abi = AbiKind::HOST_LEAF,
                     const CallResolver &resolve_call = {},
                     const VregEntries &ent = {},
                     const CallResolver &resolve_native = {});

} // namespace jit

#endif // VESTA_JIT_VREG_SELECT_H
