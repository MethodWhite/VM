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
#include "ir/ssa_ir_serialize.h"
#include "vex/lexer.h"
#include "vex/lowering.h"
#include "vex/mermaid_diagrams.h"
#include "vex/graphviz_diagrams.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include "port/transpiler_base.h"
#include "port/c/c_backend.h"

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

        // : copiar las "expectaciones" de @Macro capturadas
        // por el TypeChecker (al evaluar via AST) hacia la CompileResult.
        // El caller (main.cpp probe / shadow-eval mode) las reaplica
        // sobre una nueva ComptimeRuntime tras cargar el bytecode y
        // ejecuta shadow_validate.  Cero coste si no hay @Macros.
        const auto &ctr_ro = tc.comptime_runtime();
        const size_t n_exp = ctr_ro.expectation_count();
        res.macro_expectations.reserve(n_exp);
        for (size_t i = 0; i < n_exp; ++i) {
            auto v = ctr_ro.expectation_at(i);
            if (!v.macro_name || !v.args || !v.expected_str || !v.src_loc) {
                continue;
            }
            CompileResult::MacroExpectation e;
            e.macro_name   = *v.macro_name;
            e.args         = *v.args;
            e.expected_str = *v.expected_str;
            e.src_loc      = *v.src_loc;
            res.macro_expectations.push_back(std::move(e));
        }

        // 2.5. (opcional) Diagrama Mermaid del AST post type-check.  Lo
        // generamos AHORA porque ya tenemos los result_type rellenos pero
        // antes de que el lowering altere el AST.  Util para ver la
        // estructura del programa Vex (clases, herencia, anotaciones,
        // tipos resueltos) sin saturar con detalles de lowering.
        if (opts.dump_mermaid_ast) {
            res.mermaid_ast = mermaid_from_ast(*mod);
        }
        // Variante Graphviz del AST.  Se llena en paralelo si el flag
        // correspondiente esta activo.  Comparte el mismo punto de entrada
        // (post type-check, pre lowering) para garantizar paridad de info
        // con la version Mermaid.
        if (opts.dump_graphviz_ast) {
            res.graphviz_ast = graphviz_from_ast(*mod);
        }

        // 3. Lowering: AST -> ir::IrModule.  Pasamos el TypeChecker para
        // que el lowering pueda consultar StructLayout (offsets/tamanos)
        // sin recalcularlos.
        ir::IrModule irmod;
        Lowering lo(*mod, tc, res.diagnostics);
        if (!opts.instrument_mode.empty() && opts.instrument_mode != "none") {
            lo.set_instrument_mode(opts.instrument_mode);
        }
        const std::string mod_name = opts.module_name.empty() ? std::string("main")
                                                              : opts.module_name;
        if (!lo.run(irmod, mod_name)) {
            res.ok = false;
            return res;
        }

        /* : set @c has_lowerable_macros si el lowering emitio
         * al menos una IrFunction marcada @c is_macro_compiled.  Esto
         * es un gate mas robusto que @c macro_expectations.empty() para
         * disparar el two-phase compile, porque incluye casos con args
         * no codificables (string, struct, array) que no aparecerian
         * como expectations pero si pueden beneficiarse del VM path. */
        for (const auto &fn : irmod.functions) {
            if (fn.is_macro_compiled) {
                res.has_lowerable_macros = true;
                break;
            }
        }

        /* +: copiar las razones de skip del lowering a la
         * CompileResult para que main.cpp las imprima via VESTA_VERBOSE. */
        res.macro_skip_reasons = lo.macro_skip_reasons();

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
        // 3.5. (opcional) Diagrama Mermaid del IR pre-optimizacion.  Lo
        // generamos ANTES de tocar irmod para capturar el output crudo
        // del lowering (todos los PHIs, todos los CONSTs, blocks como
        // los emite el frontend, sin las transformaciones del optimizer).
        if (opts.dump_mermaid_ir_pre) {
            res.mermaid_ir_pre = mermaid_from_ir_module(
                irmod, "Diagrama IR pre-optimizacion (frontend output)");
        }
        if (opts.dump_graphviz_ir_pre) {
            res.graphviz_ir_pre = graphviz_from_ir_module(
                irmod, "Diagrama IR pre-optimizacion (frontend output)");
        }
        // Si CUALQUIERA de las opciones pide informacion post-opt
        // (ir_text dump, mermaid_post o graphviz_post, port target),
        // generamos UNA copia optimizada y la reusamos para todos los
        // outputs.  Asi evitamos optimizar 2-3 veces el mismo IrModule
        // cuando el usuario pide multiples formatos.
        const bool need_post_opt = opts.dump_ir
                                || opts.dump_mermaid_ir_post
                                || opts.dump_graphviz_ir_post
                                || !opts.port_target.empty();
        if (need_post_opt) {
            ir::IrModule irmod_opt = irmod;
            ir::ir_optimize(irmod_opt, opt_level_from_int(opts.opt_level));
            if (opts.dump_ir) {
                std::ostringstream ir_oss;
                ir_oss << "// ============================================\n";
                ir_oss << "// SSA IR pre-optimizacion (frontend output)\n";
                ir_oss << "// ============================================\n";
                ir::ir_print(irmod, ir_oss);
                ir_oss << "\n// ============================================\n";
                ir_oss << "// SSA IR post-optimizacion (opt_level=" << opts.opt_level << ")\n";
                ir_oss << "// ============================================\n";
                ir::ir_print(irmod_opt, ir_oss);
                res.ir_text = ir_oss.str();
            }
            const std::string title = "Diagrama IR post-optimizacion (opt_level="
                                    + std::to_string(opts.opt_level) + ")";
            if (opts.dump_mermaid_ir_post) {
                res.mermaid_ir_post = mermaid_from_ir_module(irmod_opt, title);
            }
            if (opts.dump_graphviz_ir_post) {
                res.graphviz_ir_post = graphviz_from_ir_module(irmod_opt, title);
            }

            // Port transpiler: convierte el IR optimizado a codigo fuente
            // del lenguaje destino (C, futuro: Java, JS, etc.).  Se ejecuta
            // ANTES de emitir el .vel para que el port reciba el mismo IR
            // que el bytecode (con opt_level aplicado).
            if (!opts.port_target.empty()) {
                if (opts.port_target == "c") {
                    port::PortOptions popts = opts.port_options;
                    // Si el caller no lleno module_name, usar el del compiler.
                    if (popts.module_name.empty()) {
                        popts.module_name = mod_name;
                    }
                    if (popts.source_path.empty()) {
                        popts.source_path = filename;
                    }
                    port::CBackend backend(popts);
                    port::Transpiler tx(irmod_opt, popts, backend);
                    port::TranspileResult ptres = tx.run();
                    if (ptres.ok) {
                        res.port_text     = std::move(ptres.source_text);
                        res.port_warnings = std::move(ptres.warnings);
                    } else {
                        SourceLoc loc;
                        loc.file = filename;
                        for (const auto &e : ptres.errors) {
                            res.diagnostics.error(SourceLoc{filename, 0, 0},
                                std::string("port-c: ") + e);
                        }
                    }
                } else {
                    SourceLoc loc;
                    loc.file = filename;
                    res.diagnostics.error(std::move(loc),
                        std::string("port: target '") + opts.port_target
                        + "' no soportado.  Targets validos: c");
                }
            }
        }

        // 4. Emitir IR -> texto .vel.  Aqui es donde el optimizador IR
        // hace DCE / copy prop / etc segun opt_level y el regalloc lineal
        // asigna r0..r15 a los IrValue.
        ir::EmitOptions emit_opts;
        emit_opts.opt_level     = opt_level_from_int(opts.opt_level);
        emit_opts.emit_comments = true;
        emit_opts.emit_debug    = opts.emit_debug;
        emit_opts.module_name   = mod_name;

        /* serializar el IR OPTIMIZADO a bytes para que el JIT compile la
         * version post-optimizacion (incluyendo dead-alloc elim, DCE,
         * etc.) en lugar de la version frontend cruda.
         *
         * D.7.opt: corregido bug donde el .velb llevaba IR
         * UNOPTIMIZED.  El JIT auto-trigger compilaba @c pruebas con la
         * call a @c __new_Calculadora aunque el optimizer ya la habia
         * eliminado a nivel .vel.
         *
         * Genera @c irmod_opt aplicando el mismo opt_level que el .vel
         * pipeline para mantener consistencia entre @c .velb (JIT
         * source) y @c .vel (interp source). */
        {
            ir::IrModule irmod_for_section = irmod;
            ir::ir_optimize(irmod_for_section, opt_level_from_int(opts.opt_level));
            res.ir_section_bytes = ir::emit_ir_section(irmod_for_section.functions);
        }

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

        // 5. (opcional) Diagrama Mermaid del bytecode .vel final.  Independiente
        // de las opciones IR/AST: parsea el texto del .vel para detectar labels
        // (bloques) y saltos.  Util para ver el output ultimo del frontend
        // antes de pasar al ensamblador, incluyendo opt como cmpjmp.cc, decjnz,
        // gcallocp que no aparecen en el IR (son fusion del emisor).
        if (opts.dump_mermaid_vel) {
            res.mermaid_vel = mermaid_from_vel_text(res.vel_text);
        }
        if (opts.dump_graphviz_vel) {
            res.graphviz_vel = graphviz_from_vel_text(res.vel_text);
        }

        res.ok       = !res.diagnostics.has_errors();
        return res;
    }

} // namespace vex
