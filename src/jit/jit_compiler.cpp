/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_compiler.cpp
 * @brief Implementacion del @c JitCompiler.
 *
 * Pipeline (~50 LOC pero coordina 4 subsystems):
 *   1. Selector: IR -> MFunction (con stackmaps).
 *   2. X86Encoder: MFunction -> bytes maquina + resolve fixups.
 *   3. CodeCache.alloc + memcpy + commit.
 *   4. JitRegistry.register_function con los stackmaps.
 *
 * Si cualquier paso falla, libera lo alocado y retorna @c CompileResult
 * con fn=nullptr.
 */

#include "jit/jit_compiler.h"

#include "ir/ssa_ir.h"
#include "jit/jit_registry.h"
#include "jit/machine_ir.h"
#include "jit/x86_encoder.h"

#include <cstring>
#include <utility>
#include <vector>

namespace jit {

    CompileResult JitCompiler::compile(const ir::IrFunction &ir_fn,
                                       SelectorMode mode) noexcept {
        SelectorOptions opts;
        opts.mode = mode;
        opts.runtime = &rt_;  /* D.3-B: para resolver IrOp::CALL a vrt_* */
        if (mode == SelectorMode::VM_ABI && rt_.safepoint_handler) {
            opts.safepoint_handler_addr =
                reinterpret_cast<uint64_t>(rt_.safepoint_handler);
        }
        return compile_with_opts(ir_fn, std::move(opts));
    }

    CompileResult JitCompiler::compile_with_opts(const ir::IrFunction &ir_fn,
                                                  SelectorOptions opts) noexcept {
        CompileResult result{};

        /* 1. Selector con opts custom. */
        Selector sel(std::move(opts));
        MFunction mf = sel.select(ir_fn, &result.unsupported);
        if (result.unsupported) {
            return result;
        }

        /* 2. Encoder. */
        X86Encoder enc;
        std::vector<uint8_t> bytes;
        const size_t n = enc.encode(mf, bytes);
        if (n == 0 || bytes.empty()) {
            return result;
        }
        result.instr_count = enc.instr_count();

        /* 3. Code cache. */
        uint8_t *code = cache_.alloc(bytes.size(), 16);
        if (!code) {
            return result;
        }
        std::memcpy(code, bytes.data(), bytes.size());
        cache_.commit(code, bytes.size());

        result.code_start = code;
        result.code_size  = bytes.size();
        result.fn         = reinterpret_cast<JitFn>(code);

        /* 4. Registry: registrar con los stackmaps movidos.
         * El registry usa los stackmaps para precise GC scan de los
         * frames de esta funcion. */
        JitRegistry::instance().register_function(
            code,
            code + bytes.size(),
            std::move(mf.stackmaps),
            mf.stack_frame_size,
            ir_fn.name.c_str());

        return result;
    }

    void JitCompiler::invalidate(const CompileResult &res) noexcept {
        if (!res.code_start) return;
        /* Eliminar del registry primero (asi el GC no vera frames de
         * esta fn aunque siga viva por accidente en stack). */
        JitRegistry::instance().unregister_function(res.code_start);
        /* Rellenar con INT3 para que cualquier salto/call al codigo
         * invalidado crashee controladamente con SIGTRAP. */
        cache_.invalidate(const_cast<uint8_t *>(res.code_start), res.code_size);
    }

} // namespace jit
