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
 * @file compiler.h
 * @brief Facade del compilador Vex: encadena los pases lex/parse/check/lower/emit.
 *
 * Esta es la interfaz que el dispatcher CLI llama cuando recibe
 * @c --vex archivo.vex.  Convierte el codigo fuente .vex en texto
 * .vel listo para ser pasado al ensamblador del proyecto.
 *
 * No realiza I/O por si misma (lo hace el caller); solo opera sobre
 * cadenas en memoria.  Esto facilita los tests unitarios sin tocar
 * el sistema de ficheros.
 */

#ifndef VEX_COMPILER_H
#define VEX_COMPILER_H

#include <string>

#include "vex/diagnostic.h"

namespace vex {

    /**
     * @struct CompileOptions
     * @brief Opciones del compilador Vex.
     */
    struct CompileOptions {
        std::string module_name;        ///< Nombre logico del modulo (por defecto "main").
        bool        emit_debug = false; ///< Emitir comentarios @line N en el .vel generado.
        int         opt_level  = 2;     ///< 0..3, mapea a ir::OptLevel.  Default O2 (DCE + copy prop + const fold + unreachable + TCO).
        /// si true, ademas del .vel, generar el dump
        /// textual del @c IrModule completo (post-optimizacion) en
        /// @c CompileResult::ir_text.  Util para inspeccionar lo que el
        /// frontend produce antes del backend, debug y verificacion de
        /// fixes (PHIs, SSA, CALLCLOSURE, etc.).
        bool        dump_ir = false;
    };

    /**
     * @struct CompileResult
     * @brief Resultado de la compilacion Vex.
     *
     * @c ok == true implica @c vel_text valido y diagnosticos sin errores
     * (puede haber warnings).
     * @c ok == false implica errores en diagnostics; @c vel_text puede
     * estar vacio o ser parcial.
     */
    struct CompileResult {
        bool        ok = false;     ///< Exito global.
        std::string vel_text;       ///< Texto .vel generado a partir del IR.
        std::string ir_text;        ///< dump del IrModule (solo si CompileOptions::dump_ir).
        Diagnostics diagnostics;    ///< Errores y warnings acumulados.
    };

    /**
     * @brief Compila una cadena .vex a texto .vel.
     *
     * Pipeline interno:
     *
     *   .vex source
     *     -> Lexer (token stream)
     *     -> Parser  (AST)
     *     -> TypeChecker (rellena result_type, valida)
     *     -> Lowering (AST -> ir::IrModule)
     *     -> ir::ir_emit_module (IR -> texto .vel)
     *
     * No aplica VPP; el caller debe haberlo aplicado antes si quiere
     * habilitar la metaprogramacion.
     *
     * @param source   Codigo fuente Vex.
     * @param filename Nombre logico del fichero para diagnosticos.
     * @param opts     Opciones de compilacion.
     * @return CompileResult con el .vel y el set de diagnosticos.
     */
    CompileResult compile_vex_source(const std::string &source,
                                     const std::string &filename,
                                     const CompileOptions &opts = {});

} // namespace vex

#endif // VEX_COMPILER_H
