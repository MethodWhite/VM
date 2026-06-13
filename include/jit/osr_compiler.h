/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_compiler.h
 * @brief Bridge OSR entre el interprete y el compilador JIT (Phase D.5).
 *
 * = Diseno =
 *
 * Cuando el monitor OSR detecta un bucle caliente, solicita la compilacion
 * del cuerpo del bucle como una funcion independiente (o como un segundo
 * entry point dentro de la funcion completa).  Este modulo:
 *
 *   1. Extrae el cuerpo del bucle (desde loop header hasta back-edge)
 *      como un ir::IrFunction autonomo.
 *
 *   2. Decide si compilar con C1 (rapido, poca optimizacion) o C2
 *      (agresivo, mas lento) basado en la hotness del bucle.
 *
 *   3. Compila via JitCompiler y obtiene un OsrEntry con el offset
 *      del entry point dentro del codigo generado.
 *
 *   4. Registra el OsrEntry en la tabla global para que el monitor
 *      OSR pueda encontrarlo.
 *
 *   5. Emite stackmaps para GC en el punto OSR entry.
 */

#ifndef VESTA_JIT_OSR_COMPILER_H
#define VESTA_JIT_OSR_COMPILER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/osr_data.h"
#include "jit/runtime_entries.h"

namespace ir { struct IrFunction; struct IrModule; }

namespace jit {

    /**
     * @brief Tipo de compilacion para OSR.
     */
    enum class OsrCompileTier : uint8_t {
        C1_INTERPRETED = 0,  ///< No compilar, seguir interpretando
        C1_LOOP        = 1,  ///< Compilacion rapida (C1-like)
        C2_OPTIMIZED   = 2,  ///< Compilacion optimizada (C2-like)
    };

    /**
     * @struct OsrCompileRequest
     * @brief Solicitud de compilacion OSR.
     */
    struct OsrCompileRequest {
        uint64_t    function_vaddr = 0;  ///< Direccion VM del bytecode de la funcion
        uint64_t    loop_header_pc = 0;  ///< PC del loop header
        uint32_t    loop_id        = OSR_INVALID_ID;
        uint32_t    back_edge_count = 0; ///< Contador que disparo la solicitud

        /// IDs de los valores SSA que estan vivos en el loop header.
        /// El compilador OSR debe asegurarse de que estos valores sean
        /// accesibles desde el OSR entry.
        std::vector<uint32_t> live_vids;

        /// Las capturas reales que el C1 escribio al buffer (red de
        /// seguridad del C2).  Vacio si no hay C1 previo.
        std::vector<uint32_t> captured_vids;
    };

    /**
     * @struct OsrCompileResult
     * @brief Resultado de una compilacion OSR.
     */
    struct OsrCompileResult {
        uint64_t    osr_entry_address = 0;  ///< Direccion absoluta del entry OSR
        uint32_t    osr_entry_offset  = 0;  ///< Offset dentro del codigo compilado
        OsrEntry    entry;                  ///< Entrada OSR completa
        bool        success = false;        ///< True si la compilacion fue exitosa
    };

    /**
     * @class OsrCompiler
     * @brief Compilador de bucles para OSR.
     *
     * Extrae el cuerpo del bucle, lo compila con C1 o C2, y produce
     * un OsrEntry listo para registrar.
     */
    class OsrCompiler {
    public:
        /**
         * @param cache Code cache para alocar el codigo compilado.
         * @param rt    Runtime entries para el compilador JIT.
         */
        OsrCompiler(CodeCache &cache, const RuntimeEntries &rt) noexcept
            : cache_(cache), rt_(rt) {}

        /**
         * @brief Compila un bucle para OSR en el tier indicado.
         *
         * @param ir_fn   Funcion IR completa que contiene el bucle.
         * @param req     Solicitud de compilacion (loop_id, header, vids).
         * @param tier    Tier de compilacion (C1 o C2).
         * @param resolve_user_fn  Resolver de llamadas a funciones de usuario.
         * @return Resultado de la compilacion.
         */
        OsrCompileResult compile_loop(
            const ir::IrFunction &ir_fn,
            const OsrCompileRequest &req,
            OsrCompileTier tier = OsrCompileTier::C1_LOOP,
            std::function<uint64_t(const std::string &)> resolve_user_fn = {});

        /**
         * @brief Extrae el cuerpo del bucle como un ir::IrFunction autonomo.
         *
         * El cuerpo incluye desde el loop header hasta el back-edge.
         * Los valores que entran desde fuera del bucle (parametros del
         * bucle: tipicamente los operandos PHI) se convierten en
         * parametros de la nueva funcion.
         *
         * @param source   Funcion original que contiene el bucle.
         * @param header_block  ID del bloque loop header.
         * @param out_body Funcion destino (se sobrescribe).
         * @return true si la extraccion fue exitosa.
         */
        static bool extract_loop_body(const ir::IrFunction &source,
                                      uint32_t header_block,
                                      ir::IrFunction &out_body);

        /**
         * @brief Determina el tier de compilacion basado en la hotness.
         *
         * Bucles con mas iteraciones reciben C2; bucles moderados reciben
         * C1; bucles frios no se compilan.
         */
        static OsrCompileTier select_tier(uint32_t back_edge_count);

        /**
         * @brief Registra un OsrEntry en la tabla global.
         */
        static void register_osr_entry(const OsrEntry &entry);

    private:
        CodeCache            &cache_;
        const RuntimeEntries &rt_;

        /**
         * @brief Compila con opciones C1 (loop simple).
         */
        OsrCompileResult compile_c1(const ir::IrFunction &loop_body,
                                    const OsrCompileRequest &req);

        /**
         * @brief Compila con opciones C2 (loop optimizado).
         */
        OsrCompileResult compile_c2(const ir::IrFunction &loop_body,
                                    const OsrCompileRequest &req,
                                    std::function<uint64_t(const std::string &)> resolve_user_fn);
    };

} // namespace jit

#endif // VESTA_JIT_OSR_COMPILER_H
