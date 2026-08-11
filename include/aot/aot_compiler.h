/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/aot_compiler.h
 * @brief Compilador AOT (Ahead-of-Time) para Vex bytecode.
 *
 * = Tres tiers =
 *
 *   - Full:    incluye runtime completo (GC, scheduler, async).  ~3-5 MB.
 *   - Embed:   mini-runtime sin GC ni distribucion.  ~500 KB - 1 MB.
 *   - Bare:    freestanding, sin runtime.  ~50-200 KB.
 *
 * = Formatos de salida =
 *
 *   - PE (Windows)
 *   - ELF (Linux)
 *   - Mach-O (macOS)
 *
 * = Pipeline =
 *
 *   1. Optimizar IR (ir_optimize)
 *   2. Asignar registros (allocate_regs)
 *   3. Seleccionar instrucciones (selector)
 *   4. Emitir codigo nativo (x86_encoder)
 *   5. Emitir objeto ELF (ElfEmitter)
 *   6. Linkar con runtime segun tier
 */

#ifndef AOT_AOT_COMPILER_H
#define AOT_AOT_COMPILER_H

#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace aot {

    /**
     * @brief Tier de compilacion AOT.
     *
     * Determina que partes del runtime se incrustan en el ejecutable final.
     */
    enum class Tier : uint8_t {
        FULL  = 0,  ///< Runtime completo: GC, scheduler, async, distribucion.
        EMBED = 1,  ///< Mini-runtime: no GC, no distribucion, solo async basico.
        BARE  = 2,  ///< Sin runtime: freestanding, para kernels/embedded.
    };

    /**
     * @brief Formato de archivo objeto/binario de salida.
     */
    enum class OutputFormat : uint8_t {
        PE    = 0,  ///< Portable Executable (Windows).
        ELF   = 1,  ///< ELF64 (Linux).
        MACHO = 2,  ///< Mach-O 64 (macOS).
    };

    /**
     * @brief Resultado de la compilacion AOT.
     */
    struct AotResult {
        bool ok = false;                   ///< true si la compilacion fue exitosa.
        std::string error;                 ///< descripcion del error si !ok.

        std::vector<uint8_t> object_data;  ///< bytes del archivo objeto generado.
        std::vector<uint8_t> executable;   ///< bytes del ejecutable final (si se linkeo).

        size_t code_size     = 0;          ///< bytes de codigo maquina (.text).
        size_t data_size     = 0;          ///< bytes de datos (.data + .rodata).
        size_t total_size    = 0;          ///< tamano total estimado en disco.

        /// Mapa de nombre de funcion -> offset en .text.
        std::unordered_map<std::string, uint64_t> symbol_offsets;
    };

    /**
     * @brief Opciones de configuracion del compilador AOT.
     */
    struct AotOptions {
        Tier         tier           = Tier::FULL;
        OutputFormat output_format  = OutputFormat::ELF;
        ir::OptLevel opt_level      = ir::OptLevel::O2;
        bool         strip_syms     = false;
        bool         gen_eh_frame   = true;
        std::string  output_path;            ///< Ruta del archivo de salida.
        std::vector<std::string> link_libs;  ///< Librerias adicionales para linkar.
    };

    /**
     * @class AotCompiler
     * @brief Compilador Ahead-of-Time: Vex bytecode -> ejecutable nativo.
     *
     * Orquesta el pipeline completo de compilacion:
     * optimizacion IR, asignacion de registros, seleccion de instrucciones,
     * emision de codigo maquina, generacion de objeto ELF, y linkado.
     */
    class AotCompiler {
    public:
        AotCompiler() = default;
        ~AotCompiler() = default;

        AotCompiler(const AotCompiler &) = delete;
        AotCompiler &operator=(const AotCompiler &) = delete;

        /**
         * @brief Configura el tier de compilacion.
         * @param t  Tier deseado (FULL, EMBED, BARE).
         */
        void set_tier(Tier t) noexcept { options_.tier = t; }

        /**
         * @brief Configura el formato de salida.
         * @param fmt Formato (ELF, PE, Mach-O).
         */
        void set_output_format(OutputFormat fmt) noexcept { options_.output_format = fmt; }

        /**
         * @brief Configura las opciones de compilacion.
         * @param opts Opciones completas.
         */
        void set_options(const AotOptions &opts) noexcept { options_ = opts; }

        /**
         * @brief Obtiene las opciones actuales.
         * @return Referencia a las opciones.
         */
        const AotOptions &options() const noexcept { return options_; }

        /**
         * @brief Compila un modulo IR a codigo nativo.
         *
         * Pipeline completo:
         *   1. Optimizar modulo (ir_optimize).
         *   2. Procesar cada funcion: regalloc, selector, encoder.
         *   3. Emitir archivo objeto (ELF).
         *   4. (Opcional) Linkar con runtime.
         *
         * @param mod Modulo IR a compilar.
         * @return Resultado con los bytes generados y metadatos.
         */
        AotResult compile(const ir::IrModule &mod);

    private:
        AotOptions options_;
        /// Nombre de la funcion en compilacion (para resolver self-recursion
        /// en CALLs a si misma, cuyo offset aun no esta en sym_offsets).
        std::string current_fn_name_;

        /**
         * @brief Compila una funcion individual a codigo maquina.
         * @param fn   Funcion IR.
         * @param code Buffer donde se anade el codigo generado.
         * @param sym_offsets Mapa de offset por nombre de funcion.
         * @return true si la compilacion fue exitosa.
         */
        bool compile_function(const ir::IrFunction &fn,
                              std::vector<uint8_t> &code,
                              std::unordered_map<std::string, uint64_t> &sym_offsets,
                              const std::function<uint64_t(const std::string &)> &resolve_user_fn = {},
                              const std::function<uint64_t(const std::string &)> &resolve_native_fn = {},
                              const std::function<uint64_t(const std::string &)> &resolve_symbol = {},
                              std::vector<std::pair<size_t, uint64_t>> *out_user_call_sites = nullptr);

        /**
         * @brief Genera el prologo de inicializacion segun el tier.
         * @param code Buffer donde se anade el codigo.
         */
        void emit_startup_stub(std::vector<uint8_t> &code);

        /**
         * @brief Genera la tabla de funciones runtime resueltas.
         * @return Lista de pares (nombre, direccion).
         */
        std::vector<std::pair<std::string, uint64_t>> resolve_runtime_symbols();
    };

} // namespace aot

#endif // AOT_AOT_COMPILER_H
