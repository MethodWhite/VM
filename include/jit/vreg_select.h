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
        uint64_t newobj     = 0;  ///< vrt_newobj_handle(proc, cls) -> GcHandle (NEWOBJ)
        uint64_t calln      = 0;  ///< vrt_calln(proc, lib_id, fn_id) -- FFI native
        /* Class registry (Fase 2).  Todos 1-arg (proc, params_vaddr) salvo
         * deffield/defmethod (2-arg: cls, params) y addadvice (3-arg). */
        uint64_t findclass  = 0;  ///< vrt_findclass(proc, params) -> ClassInfo*
        uint64_t findmethod = 0;  ///< vrt_findmethod(proc, params) -> MethodInfo*
        uint64_t findfield  = 0;  ///< vrt_findfield(proc, params) -> FieldInfo*
        uint64_t defclass   = 0;  ///< vrt_defclass(proc, params) -> ClassInfo*
        uint64_t setmethdbg = 0;  ///< vrt_setmethdbg(proc, params) -> void
        uint64_t deffield   = 0;  ///< vrt_deffield(proc, cls, params) -> i32
        uint64_t defmethod  = 0;  ///< vrt_defmethod(proc, cls, params) -> u32
        uint64_t addadvice  = 0;  ///< vrt_addadvice(proc, target, advice, kind) -> i32
        /* String ops (cluster cobertura 2026-06-09).  Todas via CALL al
         * runtime; STRMAKE/STRCAT devuelven GcHandle (root de tipo HANDLE). */
        uint64_t str_make   = 0;  ///< vrt_str_make(proc, vm_addr, byte_len) -> handle
        uint64_t str_len    = 0;  ///< vrt_str_len(proc, handle) -> i64 (code points)
        uint64_t str_cat    = 0;  ///< vrt_str_cat(proc, a, b) -> handle (ROPE)
        uint64_t str_cmp    = 0;  ///< vrt_str_cmp(proc, a, b) -> i64 (-1/0/1)
        uint64_t str_raw    = 0;  ///< vrt_str_raw(proc, handle) -> host_ptr a data[]
        uint64_t str_get_bytes = 0; ///< vrt_str_get_bytes(proc, handle) -> i64 byte_len
        uint64_t call_bc_function = 0; ///< vrt_call_bc_function(proc, vm_addr) -- deleter dinamico
        uint64_t callclosure = 0; ///< vrt_callclosure(proc, fn_addr, env_addr) -> result
        /* Fallback page-miss de LOAD_VM/STORE_VM (acceso a vm_mem).  0 = no
         * disponible -> esos ops caen a fallback. */
        uint64_t vm_read_u8   = 0; ///< vrt_vm_read_u8(proc, vaddr)  -> u8
        uint64_t vm_read_u16  = 0; ///< vrt_vm_read_u16(proc, vaddr) -> u16
        uint64_t vm_read_u32  = 0; ///< vrt_vm_read_u32(proc, vaddr) -> u32
        uint64_t vm_read_u64  = 0; ///< vrt_vm_read_u64(proc, vaddr) -> u64
        uint64_t vm_write_u8  = 0; ///< vrt_vm_write_u8(proc, vaddr, val)
        uint64_t vm_write_u16 = 0; ///< vrt_vm_write_u16(proc, vaddr, val)
        uint64_t vm_write_u32 = 0; ///< vrt_vm_write_u32(proc, vaddr, val)
        uint64_t vm_write_u64 = 0; ///< vrt_vm_write_u64(proc, vaddr, val)
    };

    /**
     * @brief Selecciona MachineIR de vregs desde @p fn.
     *
     * @param fn   Funcion IR (subset soportado: ver arriba).
     * @param out  MFunction destino (se sobrescribe).
     * @param ent  Direcciones de runtime entries (callvirt/gc/raw_alloc/calln).
     * @param resolve_symbol  Resolver de simbolos del linker (Phase D.3-H):
     *             dado @c "code.s_<N>" / @c "code.<fn>" devuelve la direccion
     *             VM absoluta.  Lo usan @c STR_LIT_ADDR / @c LABEL_ADDR.  Si es
     *             nullptr o retorna 0, esos ops caen a fallback (igual que el
     *             selector de slots).
     * @return     @c true si TODOS los ops estan soportados y @p out es
     *             valido; @c false si encuentra un op fuera del subset (en
     *             ese caso @p out queda indefinido y el caller hace fallback).
     */
    bool vreg_select(const ir::IrFunction &fn, MFunction &out,
                     AbiKind abi = AbiKind::HOST_LEAF,
                     const CallResolver &resolve_call = {},
                     const VregEntries &ent = {},
                     const CallResolver &resolve_native = {},
                     const CallResolver &resolve_symbol = {});

} // namespace jit

#endif // VESTA_JIT_VREG_SELECT_H
