/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vex/comptime_vm.h
 * @brief Phase MC.2 -- ComptimeRuntime: VM dedicada que ejecuta el
 *        bytecode de @Macros lowered durante compile-time.
 *
 * Reemplaza progresivamente el evaluator AST tree-walking del
 * TypeChecker.  Aprovecha la infraestructura runtime (VM + bytecode
 * + GC + futura Phase D JIT) -- speedup esperado 10-1000x para
 * macros pesados.
 *
 * Diseno:
 *   - Singleton per-compile-session: una sola VM cross-macro.
 *   - Lazy init: solo se construye cuando el primer @Macro call
 *     "lowered" se evalua.  Cero overhead si el modulo no usa @Macros
 *     o si todos los @Macros caen al fallback AST.
 *   - Reuso: la VM persiste durante toda la compilacion; multiples
 *     @Macro calls reutilizan la misma instancia.
 *   - Shutdown limpio: destructor del TypeChecker libera la VM.
 *
 * Phase MC.2 (este sprint): scaffolding -- la clase existe, gestiona
 * lifecycle, pero NO ejecuta nada todavia (placeholder hasta MC.3+).
 *
 * Phase MC.3+: marshalling args + ejecucion + read return + cache.
 */

#ifndef VEX_COMPTIME_VM_H
#define VEX_COMPTIME_VM_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vex {

    /* Pimpl interno -- la VM real se inicializa en MC.3.  En MC.2
     * la clase es solo scaffolding y el puntero queda nullptr. */
    struct ComptimeVmImpl;

    /**
     * @class ComptimeRuntime
     * @brief Singleton per-compile que aloja la VM dedicada a ejecutar
     *        @Macros lowered al IR.
     *
     * Estado interno:
     *   - vm_: la instancia runtime::VM (puntero lazy).
     *   - macro_entry_pc_: mapa de nombre canonico (`__macro_<original>`)
     *     a la entry PC dentro del bytecode cargado.  Permite call por
     *     nombre rapido.
     *   - cache_: cache de resultados (macro_name + args_hash -> result)
     *     paralelo al `pure_macro_cache_` del TypeChecker.  Habilita
     *     short-circuit sin invocar la VM en hits.
     *   - call_count_: telemetria.
     *
     * Lifecycle:
     *   - Construido como miembro del TypeChecker (default-init).
     *   - Lazy: el primer call a `try_invoke()` instancia la VM.
     *   - Destructor libera la VM y todos los recursos.
     *
     * Thread-safety: NO thread-safe en MC.2.  Compile es single-thread
     * por diseno (incluso con parallel build, cada modulo tiene su
     * propio TypeChecker -> su propio ComptimeRuntime).  MC.9 anyadira
     * pool de VMs para parallelizar @Macro calls cross-module.
     */
    class ComptimeRuntime {
    public:
        /**
         * @brief Construye un ComptimeRuntime no inicializado.  La VM
         * interna NO se crea hasta el primer call a @c try_invoke().
         */
        ComptimeRuntime() noexcept;

        /**
         * @brief Destructor.  Libera la VM si fue inicializada.  Safe
         * si nunca se uso (no-op).
         */
        ~ComptimeRuntime();

        /* No copyable/movable: la VM interna es un recurso unico. */
        ComptimeRuntime(const ComptimeRuntime &)             = delete;
        ComptimeRuntime &operator=(const ComptimeRuntime &)  = delete;
        ComptimeRuntime(ComptimeRuntime &&)                  = delete;
        ComptimeRuntime &operator=(ComptimeRuntime &&)       = delete;

        /**
         * @brief @c true si la VM ha sido inicializada (al menos un
         * @Macro lowered se invoco).  Util para diagnostico + stats.
         */
        bool is_initialized() const noexcept { return impl_ != nullptr; }

        /**
         * @brief Numero de invocaciones de @Macros que llegaron a la VM
         * (no cuenta los hits del cache externo del TypeChecker).
         */
        uint64_t call_count() const noexcept { return call_count_; }

        /**
         * @brief Numero de hits del cache interno del ComptimeRuntime.
         * Acumulado cross-macros.
         */
        uint64_t cache_hit_count() const noexcept { return cache_hit_count_; }

        /**
         * @brief placeholder -- intenta invocar el @Macro
         * @c macro_name por nombre canonico (formato `__macro_<original>`).
         *
         * retorna siempre @c false (no implementado todavia).
         * El caller debe caer al evaluator AST como fallback.
         *
         * En MC.3+ esta funcion:
         *   1. Verifica que el macro esta registrado (en @c macro_entry_pc_).
         *   2. Marshala args al estado de la VM.
         *   3. Lazy-init la VM si es la primera invocacion.
         *   4. Ejecuta call.
         *   5. Lee el return y lo deserializa a @c out_result.
         *   6. Cachea (si @Pure).
         *
         * @param macro_name Nombre canonico (`__macro_<original>`).
         * @return @c true si la invocacion exitosa; @c false en cualquier
         *         caso de error o "no implementado todavia" (caller hace
         *         fallback al evaluator AST).
         */
        bool try_invoke(const std::string &macro_name) noexcept;

        /**
         * @brief Registra el entry PC de un @Macro lowered.  Llamado
         * por el linker cuando termina el lowering: para cada IrFunction
         * con @c is_macro_compiled=true, su direccion final en el
         * bytecode se registra aqui.
         *
         * @param macro_name Nombre canonico.
         * @param entry_pc   Direccion en el bytecode donde empieza la fn.
         */
        void register_macro(const std::string &macro_name,
                             uint64_t entry_pc);

        /**
         * @brief Numero de macros registrados (independiente de
         * @c is_initialized: el registro no fuerza init de VM).
         */
        size_t registered_macro_count() const noexcept {
            return macro_entry_pc_.size();
        }

        /**
         * @brief carga bytecode de @Macros desde buffer
         * in-memory.  NO file I/O: el caller debe haber producido los
         * bytes via linker o serializacion directa de IR.
         *
         * Triggers lazy-init de la VM si no estaba inicializada.
         * Llama @c Loader::load_executable internamente.
         *
         * @param bytecode Buffer .velb completo con `__macro_*`
         *                 lowered como funciones top-level.
         * @return @c true si la carga fue exitosa; @c false si el
         *         bytecode esta corrupto, vacio, o la VM no se pudo
         *         inicializar.  Caller debe seguir con AST evaluator
         *         en caso de fallo.
         */
        bool load_macros_from_bytes(std::vector<uint8_t> bytecode) noexcept;

        /**
         * @brief fuerza init de la VM aun sin
         * macros cargados.  Util para tests de lifecycle.  En produccion
         * la init se difiere a @c load_macros_from_bytes o @c try_invoke.
         *
         * @return @c true si la VM se inicializo correctamente.
         */
        bool force_init_for_testing() noexcept {
            ensure_vm_initialized();
            return is_initialized();
        }

        /**
         * @brief itera los macros registrados con sus
         * entry PCs.  Util para diagnostico / probe.  Devuelve pairs
         * @c {macro_name, entry_pc} en orden arbitrario.
         */
        std::vector<std::pair<std::string, uint64_t>>
        list_registered_macros() const {
            std::vector<std::pair<std::string, uint64_t>> out;
            out.reserve(macro_entry_pc_.size());
            for (const auto &kv : macro_entry_pc_) {
                out.emplace_back(kv.first, kv.second);
            }
            return out;
        }

        /**
         * @brief invoca un @Macro y devuelve R0 (raw u64).
         *
         * Para macros que retornan @c i64 / @c u64, R0 contiene el valor.
         * Para macros que retornan @c string, R0 contiene un GcHandle al
         * StringObject -- en un futuro añadira la deserializacion via STRRAW
         * para obtener el @c std::string final.
         *
         * Limitacion MC.5: una sola invocacion por @c ComptimeRuntime.
         * Tras la primera, la VM esta "spent" (proceso halted) -- llamadas
         * sucesivas devolveran @c false.  MC.6+ permitira invocaciones
         * multiples via spawn de procesos frescos.
         *
         * @param macro_name Nombre canonico (`__macro_<original>`).
         * @param args       Args en orden, mapeados a R1..R12.  Soporta
         *                   hasta 12 args (CALLVM convention).
         * @param out_r0     Receptor del valor R0 tras la ejecucion.
         * @return @c true si la invocacion fue exitosa; @c false en
         *         cualquier error.
         */
        bool invoke_simple_macro(const std::string &macro_name,
                                  const std::vector<uint64_t> &args,
                                  uint64_t &out_r0) noexcept;

        /**
         * @brief invoca un @Macro que retorna @c string y
         * extrae el contenido como @c std::string host.
         *
         * Pipeline:
         *   1. Llama @c invoke_simple_macro -> obtiene R0 (GcHandle del
         *      @c StringObject construido por la macro).
         *   2. Resuelve handle -> host_ptr via gc_heap.deref().
         *   3. Lee header @c StringObject (40 bytes): encoding, length,
         *      byte_len, hash.
         *   4. Copia @c byte_len bytes desde data[] a un @c std::string.
         *
         * @param macro_name Nombre canonico (`__macro_<original>`).
         * @param args       Args en orden, mapeados a R1..R12.
         * @param out_str    Receptor del @c std::string extraido.
         * @return @c true si exitoso; @c false si:
         *   - invoke_simple_macro fallo.
         *   - R0 es 0 (handle nulo).
         *   - El handle no corresponde a un @c StringObject vivo.
         */
        bool invoke_string_macro(const std::string &macro_name,
                                  const std::vector<uint64_t> &args,
                                  std::string &out_str) noexcept;

        /**
         * @brief variante de @c invoke_string_macro con
         * memoization opt-in.  Cache @c (name, args_hash) -> result en
         * HOST memory (no toca la VM en hits).  Util para macros @Pure
         * con args repetidos cross-call-site.
         *
         * Solo cachea si @c pure es true (caller indica si el macro es
         * @Pure).  Si pure=false, equivale a @c invoke_string_macro.
         *
         * @param macro_name Nombre canonico (`__macro_<original>`).
         * @param args       Args en orden.
         * @param out_str    Receptor del resultado.
         * @param pure       @c true si el macro fue declarado @Pure.
         * @return Igual semantica que @c invoke_string_macro.
         */
        bool invoke_string_macro_memoized(const std::string &macro_name,
                                           const std::vector<uint64_t> &args,
                                           std::string &out_str,
                                           bool pure) noexcept;

        /**hits del cache (no toca VM). */
        uint64_t memo_hit_count() const noexcept { return memo_hit_count_; }
        /** misses del cache (invoca VM). */
        uint64_t memo_miss_count() const noexcept { return memo_miss_count_; }

        /**
         * @brief marshala un @c std::string host a un
         * @c GcHandle a un StringObject vivo en el GcHeap de la VM.
         *
         * Reusa la API publica @c runtime::make_string_flat (misma ruta
         * que la instruccion STRMAKE).  El StringObject alocado es
         * non-moving (alloc_pinned -> OldGen) -> el handle se mantiene
         * valido a lo largo de cualquier GC posterior.
         *
         * Util para encodear args @c string de @Macros al subset que la
         * rama VM puede invocar.  El handle resultante se pasa como
         * uint64 en R1..R12 igual que cualquier otro arg, y el body del
         * macro lo recibe como @c i64 (lo trata como handle al
         * dereferenciar via STRRAW/STRLEN/etc.).
         *
         * @param s String host UTF-8.  Empty string OK.
         * @param out_handle Receptor del GcHandle (uint32 promoted a u64).
         * @return @c true si la alocacion fue exitosa; @c false si la
         *         VM no esta inicializada o el GcHeap esta lleno.
         */
        bool marshal_string(const std::string &s, uint64_t &out_handle) noexcept;

        /**
         * @brief registra una "expectacion" capturada por
         * el TypeChecker al evaluar un @Macro via el AST evaluator.
         *
         * Cada expectacion es una tripleta @c (macro_name, args,
         * expected_str): el resultado canonico que el AST evaluator
         * computo en compile-time.  Mas tarde, @c shadow_validate()
         * invoca via VM cada expectacion y compara el resultado.
         *
         * Si la comparacion difiere, indica un bug en la lowering del
         * @Macro a IR (o en el AST evaluator).  La discrepancia se
         * reporta al caller en el vector @c report.  Si todo coincide,
         * MC.9 podra hacer el switch AST-only -> VM-only con confianza.
         *
         * Coste: O(1) por expectacion registrada; sin file I/O.
         * Almacenada en memoria hasta el primer @c shadow_validate.
         *
         * @param macro_name   Nombre canonico (`__macro_<original>`).
         * @param args         Args del call site (uint64 cada uno).
         * @param expected_str Resultado AST que el VM debe replicar.
         * @param src_loc      Texto libre describiendo el call site
         *                     (e.g., "file.vex:line:col") para diag.
         */
        void record_expectation(const std::string &macro_name,
                                 std::vector<uint64_t> args,
                                 std::string expected_str,
                                 std::string src_loc);

        /**
         * @brief struct con el resultado de una
         * comparacion shadow.  Una entrada por expectacion validada.
         */
        struct ShadowMismatch {
            std::string macro_name;
            std::string src_loc;
            std::string expected;
            std::string got;
            std::string reason;   // "ok" si match; texto del error si no
            bool        match = false;
        };

        /**
         * @brief valida TODAS las expectaciones registradas
         * con @c record_expectation invocandolas via VM (el bytecode debe
         * estar cargado con @c load_macros_from_bytes) y comparando.
         *
         * @param report Vector de salida con UNA entrada por expectacion
         *               (match=true sin discrepancia, o false con
         *               @c reason describiendo el error).  Push_back.
         * @return Numero de mismatches detectados.  0 = todo OK.
         */
        size_t shadow_validate(std::vector<ShadowMismatch> &report) noexcept;

        /**
         * @brief numero de expectaciones registradas (no
         * cuenta las ya validadas; el vector se mantiene tras validar).
         */
        size_t expectation_count() const noexcept {
            return shadow_expectations_.size();
        }

        /**
         * @brief snapshot publico de las expectaciones
         * (formato @c (name, args, expected, src_loc)).  Util para
         * que el compiler las transfiera a su CompileResult y, mas
         * tarde, el flag @c VESTA_MC_SHADOW=1 las re-aplique sobre una
         * nueva @c ComptimeRuntime tras cargar el bytecode.
         */
        struct ExpectationView {
            const std::string           *macro_name   = nullptr;
            const std::vector<uint64_t> *args         = nullptr;
            const std::string           *expected_str = nullptr;
            const std::string           *src_loc      = nullptr;
        };
        ExpectationView expectation_at(size_t idx) const noexcept {
            ExpectationView v;
            if (idx >= shadow_expectations_.size()) return v;
            const auto &e   = shadow_expectations_[idx];
            v.macro_name    = &e.macro_name;
            v.args          = &e.args;
            v.expected_str  = &e.expected_str;
            v.src_loc       = &e.src_loc;
            return v;
        }

    private:
        /**
         * @brief Lazy init de la VM.  Llamada internamente por
         * @c try_invoke en la primera invocacion.  No-op si ya esta
         * inicializada.
         */
        void ensure_vm_initialized() noexcept;

        /// Pimpl que contiene la VM dedicada + recursos asociados.
        /// @c nullptr hasta el primer call que requiera ejecucion real.
        /// Definido en @c comptime_vm.cpp con los includes runtime
        /// completos -- evita transitive deps en este header.
        std::unique_ptr<ComptimeVmImpl> impl_;

        /// Map de macros registrados -> entry PC.
        std::unordered_map<std::string, uint64_t> macro_entry_pc_;

        uint64_t call_count_      = 0;
        uint64_t cache_hit_count_ = 0;

        /// cache de resultados memoized (@Pure macros).
        /// Key = "<macro_name>:<hex_hash_de_args>".  Value = output_str.
        std::unordered_map<std::string, std::string> memo_cache_;
        uint64_t memo_hit_count_  = 0;
        uint64_t memo_miss_count_ = 0;

        /// expectaciones capturadas por el TypeChecker
        /// (resultado AST de cada @Macro call).  Validadas con VM via
        /// @c shadow_validate cuando el bytecode esta cargado.
        struct ShadowExpectation {
            std::string           macro_name;
            std::vector<uint64_t> args;
            std::string           expected_str;
            std::string           src_loc;
        };
        std::vector<ShadowExpectation> shadow_expectations_;
    };

} // namespace vex

#endif // VEX_COMPTIME_VM_H
