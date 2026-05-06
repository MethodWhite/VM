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
 * @file compiler.cpp
 * @brief Implementacion del facade del compilador Vex.
 */

#include "vex/compiler.h"

#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"
#include "vex/lexer.h"
#include "vex/lowering.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include <sstream>
#include <utility>

namespace vex {

    /**
     * @brief Convierte un entero 0..3 al enum ir::OptLevel.
     *
     * Cualquier valor fuera de rango cae en O1 (default conservador).
     */
    static ir::OptLevel opt_level_from_int(int n) noexcept {
        switch (n) {
            case 0: return ir::OptLevel::O0;
            case 1: return ir::OptLevel::O1;
            case 2: return ir::OptLevel::O2;
            case 3: return ir::OptLevel::O3;
            default: return ir::OptLevel::O1;
        }
    }

    CompileResult compile_vex_source(const std::string &source,
                                     const std::string &filename,
                                     const CompileOptions &opts) {
        CompileResult res;

        // 1. Lexer + Parser.  Si el lexer/parser ya reportan errores no
        // tiene sentido seguir: el AST seria parcial y los pases siguientes
        // generarian falsos positivos.
        Lexer  lx(source, filename, res.diagnostics);
        Parser p(lx, res.diagnostics);
        auto mod = p.parse_program();
        if (!mod || res.diagnostics.has_errors()) {
            res.ok = false;
            return res;
        }

        // 2. TypeChecker: rellena result_type y valida semantica.
        TypeChecker tc(*mod, res.diagnostics);
        if (!tc.run()) {
            res.ok = false;
            return res;
        }

        // 3. Lowering: AST -> ir::IrModule.  Pasamos el TypeChecker para
        // que el lowering pueda consultar StructLayout (offsets/tamanos)
        // sin recalcularlos.
        ir::IrModule irmod;
        Lowering lo(*mod, tc, res.diagnostics);
        const std::string mod_name = opts.module_name.empty() ? std::string("main")
                                                              : opts.module_name;
        if (!lo.run(irmod, mod_name)) {
            res.ok = false;
            return res;
        }

        // 3.5. (opcional) Volcar el IR pre-optimizacion al campo
        // @c res.ir_text para que el caller pueda inspeccionarlo con la
        // flag @c --vex-emit-ir / @c CompileOptions::dump_ir.  Util para
        // verificar que el frontend produce SSA correcto (PHI insertado
        // tras if/else, CALLCLOSURE con func_ptr+env, etc.) antes de
        // que el optimizador y el regalloc transformen el codigo.
        //
        // Para mostrar tambien el IR DESPUES de optimizar, hacemos una
        // copia del IrModule, le aplicamos las pasadas con el opt_level
        // configurado, y volcamos esa copia.  Asi NO afectamos a irmod
        // (que el emitter optimizara internamente otra vez con el mismo
        // opt_level; idempotente).  Esto es una herramienta de debug,
        // no se ejecuta cuando @c dump_ir es false (caso comun en builds
        // de produccion).
        if (opts.dump_ir) {
            std::ostringstream ir_oss;
            ir_oss << "// ============================================\n";
            ir_oss << "// SSA IR pre-optimizacion (frontend output)\n";
            ir_oss << "// ============================================\n";
            ir::ir_print(irmod, ir_oss);
            // Copia para optimizar y mostrar el resultado tras DCE/copy
            // prop/const fold/TCO segun el opt_level configurado.
            ir::IrModule irmod_opt = irmod;
            ir::ir_optimize(irmod_opt, opt_level_from_int(opts.opt_level));
            ir_oss << "\n// ============================================\n";
            ir_oss << "// SSA IR post-optimizacion (opt_level=" << opts.opt_level << ")\n";
            ir_oss << "// ============================================\n";
            ir::ir_print(irmod_opt, ir_oss);
            res.ir_text = ir_oss.str();
        }

        // 4. Emitir IR -> texto .vel.  Aqui es donde el optimizador IR
        // hace DCE / copy prop / etc segun opt_level y el regalloc lineal
        // asigna r0..r15 a los IrValue.
        ir::EmitOptions emit_opts;
        emit_opts.opt_level     = opt_level_from_int(opts.opt_level);
        emit_opts.emit_comments = true;
        emit_opts.emit_debug    = opts.emit_debug;
        emit_opts.module_name   = mod_name;

        ir::EmitResult eres = ir::ir_emit_module(irmod, emit_opts);
        if (!eres.ok) {
            // Volcar el error del emisor al sumidero unificado.
            SourceLoc loc;
            loc.file = filename;
            res.diagnostics.error(std::move(loc),
                std::string("emisor IR fallo: ") + eres.error);
            res.ok = false;
            return res;
        }

        res.vel_text = std::move(eres.vel_text);
        res.ok       = !res.diagnostics.has_errors();
        return res;
    }

} // namespace vex
