/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/auto_jit.h
 * @brief Auto-JIT trigger: cuando un metodo se invoca
 *        repetidamente, busca su IR en el Loader y lo JIT-compila.
 *
 * = Threshold =
 *
 * El threshold global @c g_jit_threshold controla cuantas invocaciones
 * deben acumular antes de disparar la compilacion JIT.  Default:
 * @c UINT32_MAX (JIT desactivado).  El usuario lo configura via:
 *   - Env var @c VESTA_JIT_THRESHOLD=N (al inicio de la VM).
 *   - CLI flag @c --jit-threshold N (futuro).
 *   - API runtime @c set_jit_threshold(N) (testing).
 *
 * Threshold tipico: 100-1000.  Demasiado bajo = compile overhead supera
 * la ganancia del JIT (programas que no son hot pagan compile time
 * extra).  Demasiado alto = JIT nunca se dispara para loops cortos.
 */

#ifndef VESTA_JIT_AUTO_JIT_H
#define VESTA_JIT_AUTO_JIT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime { class ProcessVM; }
namespace loader  { struct MethodInfo; }
namespace ir      { struct IrFunction; }

namespace jit {

    /**
     * @brief Threshold global de invocaciones para auto-JIT.
     *        Default: UINT32_MAX (JIT desactivado).
     */
    extern uint32_t g_jit_threshold;

    /**
     * @brief Configura el threshold runtime (para tests / CLI flag).
     */
    void set_jit_threshold(uint32_t threshold) noexcept;

    /* ===================================================================== */
    /* Sistema de warnings para depuracion del JIT                            */
    /* ===================================================================== */

    /**
     * @brief Si true, el Selector imprime a stderr cada vez que encuentra
     *        una IR op no soportada, indicando funcion + op + linea fuente.
     *
     * Activado via env var @c VESTA_JIT_WARN_UNSUPPORTED=1 al inicio de
     * la VM.  Util para depurar por que una funcion concreta no se
     * JIT-compila ("ah, usa DIV que aun no implemente").
     *
     * Default: false (sin warnings, debug discretamente).
     */
    extern bool g_jit_warn_unsupported;

    /**
     * @brief Si true, tras cada compilacion exitosa se imprime a stderr
     *        el dump de las MInstrs generadas + el disasm hex/asm del
     *        codigo nativo via Capstone.  Activado por env var
     *        @c VESTA_JIT_DISASM=1.  Default: false.
     *
     * Util para depurar comportamiento incorrecto del JIT (e.g. crash
     * tras un compile exitoso): permite ver exactamente que bytes se
     * emitieron y donde el codigo nativo se desvia de lo esperado.
     */
    extern bool g_jit_disasm;

    /**
     * @brief Dumpea a stderr los bytes generados + disasm Capstone para
     *        una funcion JIT-compilada.  Llamado tras cada compile
     *        exitoso cuando @c g_jit_disasm == true.
     */
    void debug_dump_jit_code(const std::string &name,
                             const uint8_t *code,
                             size_t code_size);

    /**
     * @brief Counters globales para introspeccion del JIT:
     *   - g_jit_compiled_count: funciones JIT-compiladas exitosamente.
     *   - g_jit_unsupported_count: intentos fallidos por IR op no soportada.
     *   - g_jit_no_ir_count: intentos fallidos por IR no encontrado en
     *     Loader (e.g. .velb v2 sin seccion @ir).
     *
     * Util para auditar % de funciones JIT-eatables en programas reales.
     */
    extern uint64_t g_jit_compiled_count;
    extern uint64_t g_jit_unsupported_count;
    extern uint64_t g_jit_no_ir_count;

    /**
     * @brief Devuelve un snapshot legible del estado del JIT
     *        (counters + threshold).  Util para printear al final del
     *        programa con @c --jit-stats flag.
     */
    std::string get_jit_stats_summary();

    /**
     * @brief Maybe compila el metodo si su counter supera el threshold.
     *
     * Llamado por @c exec_instr_callvirt (fast path) en cada invocacion.
     * Coste fast path (threshold no superado): 1 cmp + 1 branch = ~1 ns.
     *
     * Flujo cuando se dispara:
     *   1. Lookup en Loader::executables del @c IrFunction con nombre
     *      @c "<ClassName>__<methodName>".
     *   2. Si encontrado, llama a @c JitCompiler::compile(*ir, VM_ABI).
     *   3. Si exito, @c method->jit_code = result.fn.
     *   4. Proxima invocacion via CALLVIRT despacha al codigo nativo
     *      (hook en exec_instr_callvirt ya implementado en D.3-C).
     *
     * Si la busqueda IR falla (programa sin IR embebido en .velb v2,
     * funcion sin IR por algun caso edge, etc.), @c method->jit_code
     * queda en nullptr y futuras invocaciones reincrementan el counter.
     * Para evitar busquedas repetidas, podriamos marcar method como
     * "JIT-failed" pero por ahora la complejidad no se justifica.
     *
     * @param vm     ProcessVM actual (para acceder a Loader via scheduler).
     * @param method MethodInfo cuyo counter acaba de incrementarse.
     */
    void maybe_compile_method(runtime::ProcessVM *vm,
                              loader::MethodInfo *method) noexcept;

    /* Forward decl de CompileResult (definido en jit_compiler.h). */
    struct CompileResult;

    /**
     * @brief Compila una @c IrFunction usando el JIT subsystem (lazy
     *        init si es la primera invocacion).
     *
     * Usado por el Loader para eager-compile de @c main, ya que main
     * no se invoca via CALLVIRT (es free function) y el hook auto-JIT
     * normal no lo dispara.  Si el JIT esta desactivado
     * (@c g_jit_threshold == UINT32_MAX), devuelve @c CompileResult{}
     * sin compilar.
     *
     * @param ir_fn La funcion IR a compilar.
     * @param ir_lookup Mapa nombre -> indice en @p ir_functions.  Si no
     *        es nullptr, habilita el resolver de CALL a user functions
     *        via recursive eager-compile.
     * @param ir_functions Vector con todas las funciones IR del ejecutable.
     *        Usado para resolver llamadas user-fn -> compile + return ptr.
     * @return Resultado de la compilacion (fn=nullptr si fallo o JIT off).
     */
    CompileResult eager_compile_function(
        const ir::IrFunction &ir_fn,
        const std::unordered_map<std::string, size_t> *ir_lookup = nullptr,
        const std::vector<ir::IrFunction> *ir_functions = nullptr,
        const std::unordered_map<std::string, uint64_t> *symbol_table = nullptr,
        std::function<uint64_t(const std::string &)> resolve_native_fn = nullptr,
        std::function<uint64_t(uint64_t)> read_vmem_u64 = nullptr);

} // namespace jit

#endif // VESTA_JIT_AUTO_JIT_H
