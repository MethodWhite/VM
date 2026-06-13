/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
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
#include "vex/html_diagrams.h"
#include "vex/namespace_flatten.h"
#include "vex/parser.h"
#include "vex/type_checker.h"

#include "port/transpiler_base.h"
#include "port/c/c_backend.h"
#include "port/wasm/wasm_backend.h"

#include <sstream>
#include <utility>

namespace vex {

    static ir::OptLevel opt_level_from_int(int n) noexcept {
        switch (n) {
            case 0: return ir::OptLevel::O0;
            case 1: return ir::OptLevel::O1;
            case 2: return ir::OptLevel::O2;
            case 3: return ir::OptLevel::O3;
            default: return ir::OptLevel::O1;
        }
    }

    // Internal context with typed access
    struct CompilerCtx {
        const std::string               *source;
        const std::string               *filename;
        const CompileOptions            *opts;
        CompileResult                   *result;
        std::unique_ptr<ast::ModuleNode> ast;
        std::unique_ptr<TypeChecker>     type_checker;
        std::unique_ptr<ir::IrModule>    irmod;
    };

    // -----------------------------------------------------------------------
    // Pass 1: Lex + Parse
    // -----------------------------------------------------------------------
    static bool pass_lex_parse(CompilerCtx &ctx) {
        Lexer  lx(*ctx.source, *ctx.filename, ctx.result->diagnostics);
        Parser p(lx, ctx.result->diagnostics);
        ctx.ast = p.parse_program();
        if (!ctx.ast || ctx.result->diagnostics.has_errors()) {
            ctx.result->ok = false;
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Pass 2: Flatten namespaces + Type check + Macro expectations + AST diagrams
    // -----------------------------------------------------------------------
    static bool pass_type_check(CompilerCtx &ctx) {
        // Flatten namespaces inline
        auto inline_namespaces = flatten_namespaces(*ctx.ast);

        // Type checking
        ctx.type_checker = std::make_unique<TypeChecker>(*ctx.ast, ctx.result->diagnostics);

        for (const auto &ins : inline_namespaces) {
            const uint32_t ns_idx = ctx.type_checker->register_imported_namespace(
                ins.name, ins.name);
            for (const auto &sym : ins.symbols) {
                TypeChecker::ImportedNamespace::Sym ns_sym;
                ns_sym.kind          = (sym.kind == FlattenedNamespace::Sym::Function) ? 0
                                     : (sym.kind == FlattenedNamespace::Sym::Type ? 2 : 1);
                ns_sym.mangled_label = sym.mangled_label;
                ctx.type_checker->register_namespace_symbol(ns_idx, sym.public_name,
                                                             std::move(ns_sym));
            }
        }

        if (!ctx.type_checker->run()) {
            ctx.result->ok = false;
            return false;
        }

        // Copy macro expectations
        const auto &ctr_ro = ctx.type_checker->comptime_runtime();
        const size_t n_exp = ctr_ro.expectation_count();
        ctx.result->macro_expectations.reserve(n_exp);
        for (size_t i = 0; i < n_exp; ++i) {
            auto v = ctr_ro.expectation_at(i);
            if (!v.macro_name || !v.args || !v.expected_str || !v.src_loc) continue;
            CompileResult::MacroExpectation e;
            e.macro_name   = *v.macro_name;
            e.args         = *v.args;
            e.expected_str = *v.expected_str;
            e.src_loc      = *v.src_loc;
            ctx.result->macro_expectations.push_back(std::move(e));
        }

        // Optional AST diagrams
        const auto &opts = *ctx.opts;
        if (opts.dump_mermaid_ast)
            ctx.result->mermaid_ast = mermaid_from_ast(*ctx.ast);
        if (opts.dump_graphviz_ast)
            ctx.result->graphviz_ast = graphviz_from_ast(*ctx.ast);
        if (opts.dump_html_ast)
            ctx.result->html_ast = html_from_ast(*ctx.ast);

        return true;
    }

    // -----------------------------------------------------------------------
    // Pass 3: Lowering AST -> IR
    // -----------------------------------------------------------------------
    static bool pass_lowering(CompilerCtx &ctx) {
        ctx.irmod = std::make_unique<ir::IrModule>();
        Lowering lo(*ctx.ast, *ctx.type_checker, ctx.result->diagnostics);
        if (!ctx.opts->instrument_mode.empty() && ctx.opts->instrument_mode != "none") {
            lo.set_instrument_mode(ctx.opts->instrument_mode);
        }
        const std::string mod_name = ctx.opts->module_name.empty() ? "main" : ctx.opts->module_name;
        if (!lo.run(*ctx.irmod, mod_name)) {
            ctx.result->ok = false;
            return false;
        }

        // Check for lowerable macros
        for (const auto &fn : ctx.irmod->functions) {
            if (fn.is_macro_compiled) {
                ctx.result->has_lowerable_macros = true;
                break;
            }
        }
        ctx.result->macro_skip_reasons = lo.macro_skip_reasons();

        // Optional IR pre-optimization diagrams
        const auto &opts = *ctx.opts;
        if (opts.dump_mermaid_ir_pre)
            ctx.result->mermaid_ir_pre = mermaid_from_ir_module(*ctx.irmod, "IR pre-opt");
        if (opts.dump_graphviz_ir_pre)
            ctx.result->graphviz_ir_pre = graphviz_from_ir_module(*ctx.irmod, "IR pre-opt");
        if (opts.dump_html_ir_pre)
            ctx.result->html_ir_pre = html_from_ir_module(*ctx.irmod, "IR pre-opt");

        return true;
    }

    // -----------------------------------------------------------------------
    // Pass 4: Optimize IR
    // -----------------------------------------------------------------------
    static bool pass_optimize_ir(CompilerCtx &ctx) {
        ir::ir_optimize(*ctx.irmod, opt_level_from_int(ctx.opts->opt_level));
        return true;
    }

    // -----------------------------------------------------------------------
    // Pass 5: Post-opt diagrams + port transpilation + .vel emit
    // -----------------------------------------------------------------------
    static bool pass_emit(CompilerCtx &ctx) {
        const auto &opts = *ctx.opts;
        const std::string mod_name = opts.module_name.empty() ? "main" : opts.module_name;

        // Serialize IR section for JIT
        {
            ir::IrModule irmod_opt = *ctx.irmod;
            ir::ir_optimize(irmod_opt, opt_level_from_int(opts.opt_level));
            ctx.result->ir_section_bytes = ir::emit_ir_section(irmod_opt.functions);

            // Post-opt diagrams and port transpilation (only if requested)
            const bool need_post = opts.dump_ir
                || opts.dump_mermaid_ir_post
                || opts.dump_graphviz_ir_post
                || opts.dump_html_ir_post
                || !opts.port_target.empty();

            if (need_post) {
                if (opts.dump_ir) {
                    std::ostringstream ir_oss;
                    ir::ir_print(*ctx.irmod, ir_oss);
                    ir_oss << "\n// ----- post-opt -----\n";
                    ir::ir_print(irmod_opt, ir_oss);
                    ctx.result->ir_text = ir_oss.str();
                }
                const std::string title = "IR post-opt (O" + std::to_string(opts.opt_level) + ")";
                if (opts.dump_mermaid_ir_post)
                    ctx.result->mermaid_ir_post = mermaid_from_ir_module(irmod_opt, title);
                if (opts.dump_graphviz_ir_post)
                    ctx.result->graphviz_ir_post = graphviz_from_ir_module(irmod_opt, title);
                if (opts.dump_html_ir_post)
                    ctx.result->html_ir_post = html_from_ir_module(irmod_opt, title);

                // Port transpilation
                if (!opts.port_target.empty()) {
                    if (opts.port_target == "c") {
                        port::PortOptions popts = opts.port_options;
                        if (popts.module_name.empty()) popts.module_name = mod_name;
                        if (popts.source_path.empty()) popts.source_path = *ctx.filename;
                        port::CBackend backend(popts);
                        port::Transpiler tx(irmod_opt, popts, backend);
                        port::TranspileResult ptres = tx.run();
                        if (ptres.ok) {
                            ctx.result->port_text     = std::move(ptres.source_text);
                            ctx.result->port_warnings = std::move(ptres.warnings);
                        } else {
                            for (const auto &e : ptres.errors) {
                                ctx.result->diagnostics.error(
                                    SourceLoc{*ctx.filename, 0, 0},
                                    std::string("port-c: ") + e);
                            }
                        }
                    } else if (opts.port_target == "wasm") {
                        port::WasmPortOptions wopts;
                        // Copy base port options
                        static_cast<port::PortOptions&>(wopts) = opts.port_options;
                        if (wopts.module_name.empty()) wopts.module_name = mod_name;
                        if (wopts.source_path.empty()) wopts.source_path = *ctx.filename;
                        if (wopts.wasm_module_name.empty()) wopts.wasm_module_name = mod_name;

                        port::WasmBackend backend(wopts);
                        port::Transpiler tx(irmod_opt, wopts, backend);
                        port::TranspileResult ptres = tx.run();

                        if (ptres.ok) {
                            // Store WASM binary as string (bytes)
                            const auto &wasm_bin = backend.binary();
                            ctx.result->port_text.assign(
                                reinterpret_cast<const char*>(wasm_bin.data()),
                                wasm_bin.size());
                            ctx.result->port_warnings = std::move(ptres.warnings);
                        } else {
                            for (const auto &e : ptres.errors) {
                                ctx.result->diagnostics.error(
                                    SourceLoc{*ctx.filename, 0, 0},
                                    std::string("port-wasm: ") + e);
                            }
                        }
                    } else {
                        ctx.result->diagnostics.error(
                            SourceLoc{*ctx.filename, 0, 0},
                            std::string("port: target '") + opts.port_target + "' no soportado");
                    }
                }
            }
        }

        // Emit .vel bytecode
        ir::EmitOptions emit_opts;
        emit_opts.opt_level     = opt_level_from_int(opts.opt_level);
        emit_opts.emit_comments = true;
        emit_opts.emit_debug    = opts.emit_debug;
        emit_opts.module_name   = mod_name;

        ir::EmitResult eres = ir::ir_emit_module(*ctx.irmod, emit_opts);
        if (!eres.ok) {
            ctx.result->diagnostics.error(
                SourceLoc{*ctx.filename, 0, 0},
                std::string("emisor IR fallo: ") + eres.error);
            ctx.result->ok = false;
            return false;
        }
        ctx.result->vel_text = std::move(eres.vel_text);

        // Optional bytecode diagrams
        if (opts.dump_mermaid_vel)
            ctx.result->mermaid_vel = mermaid_from_vel_text(ctx.result->vel_text);
        if (opts.dump_graphviz_vel)
            ctx.result->graphviz_vel = graphviz_from_vel_text(ctx.result->vel_text);
        if (opts.dump_html_vel)
            ctx.result->html_vel = html_from_vel_text(ctx.result->vel_text);

        return true;
    }

    // -----------------------------------------------------------------------
    // compile_vex_source: pipeline orchestrator
    // -----------------------------------------------------------------------
    CompileResult compile_vex_source(const std::string &source,
                                     const std::string &filename,
                                     const CompileOptions &opts) {
        CompileResult res;

        CompilerCtx ctx;
        ctx.source   = &source;
        ctx.filename = &filename;
        ctx.opts     = &opts;
        ctx.result   = &res;

        // Execute pipeline: early exit on any failure
        if (!pass_lex_parse(ctx))       return res;
        if (!pass_type_check(ctx))      return res;
        if (!pass_lowering(ctx))        return res;
        if (!pass_optimize_ir(ctx))     return res;
        if (!pass_emit(ctx))            return res;

        res.ok = !res.diagnostics.has_errors();
        return res;
    }

} // namespace vex
