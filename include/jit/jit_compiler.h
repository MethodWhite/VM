/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_compiler.h
 * @brief Compilador JIT C1 - API publica para compilar un IrFunction
 *        a codigo nativo ejecutable.
 *
 * = Pipeline =
 *
 *   IrFunction
 *      |
 *      | Selector
 *      v
 *   MFunction (con stackmaps)
 *      |
 *      | X86Encoder
 *      v
 *   bytes maquina
 *      |
 *      | CodeCache.alloc + commit
 *      v
 *   codigo nativo ejecutable
 *      |
 *      | JitRegistry.register_function
 *      v
 *   JitFn invocable + GC-aware
 *
 * = Uso tipico =
 *
 *   JitCompiler comp(code_cache, runtime_entries);
 *   JitFn fn = comp.compile(ir_fn);
 *   if (fn) {
 *       jit::enter_jit(fn, proc);
 *   }
 *
 * = Estado =
 *
 * (sin CALL support): solo funciones de aritmetica
 * pura + loops con PHI funcionan.  Cualquier IR op no soportada
 * causa @c compile() devolver nullptr (la funcion queda solo en
 * el interprete).
 */

#ifndef VESTA_JIT_JIT_COMPILER_H
#define VESTA_JIT_JIT_COMPILER_H

#include <cstdint>

#include "jit/code_cache.h"
#include "jit/interp_jit_bridge.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"

namespace ir { struct IrFunction; }

namespace jit {

    /**
     * @struct CompileResult
     * @brief Resultado de una compilacion JIT.
     */
    struct CompileResult {
        JitFn       fn          = nullptr;  ///< Puntero al codigo nativo (null si fallo)
        size_t      code_size   = 0;        ///< Bytes emitidos
        size_t      instr_count = 0;        ///< MInstrs emitidos
        bool        unsupported = false;    ///< IR contenia op no soportada
        const uint8_t *code_start = nullptr; ///< Para unregister + invalidate
    };

    /**
     * @class JitCompiler
     * @brief Compila IrFunctions a codigo nativo.
     */
    class JitCompiler {
    public:
        /**
         * @param cache     code cache donde se aloca el codigo.  Reuse
         *                  cross-funcion.
         * @param rt        runtime entries (usado por safepoint handler addr).
         */
        JitCompiler(CodeCache &cache, const RuntimeEntries &rt) noexcept
            : cache_(cache), rt_(rt) {}

        /**
         * @brief Compila @p ir_fn a codigo nativo.
         * @param mode  convencion de llamada (NATIVE_ABI para tests,
         *              VM_ABI para integracion runtime).
         * @return CompileResult con el puntero ejecutable + stats.
         *         Si compile fallo, @c fn = nullptr; @c unsupported
         *         indica si fue por IR ops no soportadas.
         *
         * La funcion compilada queda registrada en @c JitRegistry para
         * que el GC pueda hacer precise scan de sus frames.
         */
        CompileResult compile(const ir::IrFunction &ir_fn,
                              SelectorMode mode = SelectorMode::VM_ABI) noexcept;

        /**
         * @brief Compila con opciones explicitas de Selector.
         *
         * Util para casos avanzados como eager-compile con resolver de
         * user-fn.  El mode + runtime + safepoint_handler de
         * @p opts se respetan tal cual.
         */
        CompileResult compile_with_opts(const ir::IrFunction &ir_fn,
                                        SelectorOptions opts) noexcept;

        /**
         * @brief Invalida una funcion JIT previamente compilada.
         *
         * Rellena su codigo con INT3 (cualquier llamada futura crashea
         * controladamente) y la elimina del JitRegistry.
         */
        void invalidate(const CompileResult &res) noexcept;

    private:
        CodeCache            &cache_;
        const RuntimeEntries &rt_;
    };

} // namespace jit

#endif // VESTA_JIT_JIT_COMPILER_H
