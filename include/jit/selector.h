/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/selector.h
 * @brief Instruction selector @c ssa_ir::IrFunction -> @c jit::MFunction (Phase D.1.b).
 *
 * = Diseno =
 *
 * Selector C1-style "template" minimal: cada SSA value tiene un slot
 * stack fijo (offset = -8 * (vid+1) desde RBP).  Cada IrInstr emite:
 *
 *   1. LOAD operandos desde sus slots a regs scratch.
 *   2. Compute (ADD/SUB/IMUL/CMP/etc.).
 *   3. STORE resultado a slot del dst.
 *
 * Cero regalloc: el codigo emitido es sub-optimo en hot loops (cada
 * op accede memoria stack), pero CORRECTO y simple.  Phase D.3
 * (C1 con minimal regalloc) y D.7 (linear scan) lo refinaran.
 *
 * = Calling convention =
 *
 * @c SelectorMode::NATIVE_ABI (default v1):
 *   - Params en rdi/rsi/rdx/rcx/r8/r9 (SysV) o rcx/rdx/r8/r9 (Win64)
 *   - Return en rax
 *   - Permite testear el selector sin ProcessVM setup
 *
 * @c SelectorMode::VM_ABI:
 *   - rdi/rcx = ProcessVM*
 *   - args reales en proc->registers.regs[1..N]
 *   - return en proc->registers.regs[0]
 *
 * = Cobertura v1 =
 *
 * IR ops soportados: CONST, MOV, ADD, SUB, MUL (IMUL), AND, OR, XOR,
 * NEG, NOT, SHL, SHR, SAR, LOAD, STORE, CMP_EQ/NE/LT/GT/LE/GE (signed
 * y unsigned), BR, BR_COND, RET.
 *
 * No soportados (caen al interprete via fallback en D.5):
 * - PHI (emit phi copies en preds aun no implementado)
 * - CALL/CALLVIRT/CALLN/CALLCLOSURE/TAILCALL (D.3 los anyade)
 * - DIV/MOD (necesitan setup de RDX:RAX para IDIV)
 * - FADD/FSUB/FMUL/FDIV y otros float (D.3+)
 * - ALLOCA/STR_LIT_ADDR/etc.
 */

#ifndef VESTA_JIT_SELECTOR_H
#define VESTA_JIT_SELECTOR_H

#include "jit/machine_ir.h"
#include "jit/runtime_entries.h"
#include "ir/ssa_ir.h"

#include <functional>
#include <string>

namespace jit {

    /**
     * @enum SelectorMode
     * @brief Convencion de llamada para el codigo generado.
     */
    enum class SelectorMode {
        NATIVE_ABI,  ///< Convencion C nativa del host (SysV / Win64).
        VM_ABI       ///< Convencion VM (ProcessVM* + regs internos).
    };

    /**
     * @struct SelectorOptions
     * @brief Opciones de configuracion del selector.
     */
    struct SelectorOptions {
        SelectorMode mode = SelectorMode::NATIVE_ABI;
        /// Direccion absoluta de @c vrt_safepoint_handler para que el
        /// selector pueda emitir safepoints en VM_ABI mode.  Tipicamente
        /// se obtiene de @c jit::RuntimeEntries::safepoint_handler.
        /// Si es 0 en VM_ABI mode, los safepoints se omiten (modo
        /// debug/testing sin coordinacion GC).
        uint64_t safepoint_handler_addr = 0;
        /// D.3-B: tabla de runtime entries.  El selector la usa para
        /// resolver @c IrOp::CALL a @c vrt_* y emitir CALL directo a la
        /// direccion del wrapper.  Si es @c nullptr, cualquier
        /// @c IrOp::CALL no soportado marca @c unsupported=true.
        const RuntimeEntries *runtime = nullptr;

        /// D.3-E: callback opcional para resolver CALLs a funciones user
        /// (definidas en el .velb, no en runtime).  El selector la
        /// invoca cuando @c IrOp::CALL tiene un @c func_name que NO es
        /// @c vrt_* y necesita recursivamente compilar la callee.
        /// Devuelve 0 si la funcion no existe / no es compilable; sino
        /// la direccion absoluta de su codigo nativo (call rax).
        /// El callee se invoca con VM_ABI calling convention (args ya
        /// staged en @c proc->registers.regs[1..N+1]).
        /// Es responsabilidad del callback evitar recursion infinita
        /// (e.g. via cache name -> ptr, valor centinela "in progress").
        std::function<uint64_t(const std::string &)> resolve_user_fn{};

        /// D.3-H: callback opcional para resolver simbolos @Absolute("X")
        /// dentro de raw_asm a su direccion VM absoluta (resuelta por
        /// el linker).  El selector invoca este callback al parsear
        /// `mov rN, @Absolute("code.s_K")` o `mov [rN], @Absolute(...)`.
        /// Si es nullptr o retorna 0, esos patrones siguen siendo
        /// rechazados como unsupported.
        std::function<uint64_t(const std::string &)> resolve_symbol{};

        /// 2026-05-16: callback opcional para resolver funciones nativas
        /// importadas via CALLN.  Recibe el nombre completo @c "lib:func"
        /// (e.g. @c "stdlib/native/io/vesta_io:vio_println") y devuelve la
        /// direccion HOST del simbolo nativo cargado (LoadLibraryA +
        /// GetProcAddress).  Si retorna 0, la CALLN se marca @c unsupported
        /// y el metodo cae a fallback interp.  Tipicamente el caller
        /// resuelve usando @c loader::FFI::load_native_module +
        /// @c resolve_native_symbol.
        std::function<uint64_t(const std::string &)> resolve_native_fn{};

        /// Callback para reservar un slot de Inline Cache (16 bytes:
        /// cached_class_ptr + cached_jit_code, inicial 0).  Retorna la
        /// addr HOST del slot.  El selector embebe esa addr en el codigo
        /// emitido para CALLVIRT IC pattern.  Si es null, el selector
        /// emite el inline dispatch tradicional (sin IC).
        std::function<uint64_t()> reserve_ic_slot{};

        /// Callback para leer un qword (u64) de @c vm_mem en compile-time.
        /// Usado por el peephole de inlining: cuando el JIT ve
        /// @c mov rN, @Absolute("code.s_X") + @c mov rN, [rN], lee el
        /// valor cacheado en s_X (typically un @c ClassInfo* poblado por
        /// @c __module_init que ya corrio antes del JIT compile) e
        /// inlinea el constante como imm64, eliminando la llamada a
        /// @c vrt_vm_read_u64 en cada iteracion del hot path.
        /// Si retorna 0 o no esta seteado, el patron usa la ruta normal
        /// (resolver vaddr + vrt_vm_read_u64).
        std::function<uint64_t(uint64_t /*vaddr*/)> read_vmem_u64{};

        /// Offset (en bytes) desde @c ProcessVM* hasta @c gc_heap.nursery_bump_.
        /// Si != 0, el mini-parser del JIT inlinea bump-pointer allocation
        /// emitiendo @c mov rax, [rbx + nursery_bump_off] +
        /// @c lea rcx, [rax + total] + @c cmp rcx, [rbx + nursery_end_off]
        /// + @c ja slow_path + @c mov [rbx + nursery_bump_off], rcx +
        /// init GcHeader + init ObjectHeader + call @c vrt_register_alloc
        /// para registrar el handle.  Saves ~30 ns por allocacion
        /// (vs ~50 ns del call a @c vrt_newobj).
        /// Si es 0, el JIT cae al call de @c vrt_newobj normal.
        int32_t nursery_bump_offset = 0;
        int32_t nursery_end_offset  = 0;
        /// Direccion del runtime helper @c vrt_register_alloc (param 2
        /// = host_ptr al GcHeader, register handle solo).  Si es 0, el
        /// JIT no puede inlinear (fallback a @c vrt_newobj).
        uint64_t register_alloc_addr = 0;
    };

    /**
     * @class Selector
     * @brief Baja un @c IrFunction a un @c MFunction.
     */
    class Selector {
    public:
        explicit Selector(SelectorOptions opts = {}) : opts_(opts) {}

        /**
         * @brief Selecciona instrucciones para @p ir_fn.
         * @return @c MFunction listo para encoder.  Si encuentra un IR
         *         op no soportado, anyade un @c MOp::INT3 como marker y
         *         pone @p out_unsupported a true.
         */
        MFunction select(const ir::IrFunction &ir_fn, bool *out_unsupported = nullptr);

    private:
        SelectorOptions opts_;
        /// Indice en imm64_pool de la direccion del safepoint handler.
        /// Computado al inicio de @c select() si VM_ABI y handler != 0.
        /// UINT32_MAX = no disponible (safepoints omitidos).
        uint32_t safepoint_pool_idx_ = UINT32_MAX;
    };

} // namespace jit

#endif // VESTA_JIT_SELECTOR_H
