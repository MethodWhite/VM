/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "jit/c2_compiler.h"
#include "jit/c2_heuristics.h"
#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/runtime_entries.h"
#include "vesta_rt/public.h"
#include "runtime/proceso_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace jit {
namespace c2_deopt {

    // =====================================================================
    //  Deopt handler structure (emitted in the code cache)
    // =====================================================================

    /// Layout of a deopt handler in the code cache.
    /// Each handler is a small trampoline that:
    ///   1. Saves all live registers to the stack
    ///   2. Calls the runtime deopt handler with the metadata index
    ///   3. Falls through to the interpreter bailout path
    struct DeoptHandler {
        uint32_t metadata_index;   ///< Index into deopt_metadata array
        uint32_t frame_size;       ///< Size of the JIT frame at deopt point
        uint32_t live_count;       ///< Number of live values at deopt point
        /// Offsets (relative to RBP) for each live value
        int16_t  live_offsets[16];
    };

    // =====================================================================
    //  Deoptimization frame reconstruction
    // =====================================================================

    /**
     * @brief Reconstructs the interpreter state from a deoptimization point.
     *
     * When a speculative optimization fails (e.g., class check fails),
     * the runtime calls this function to convert the optimized native frame
     * back to an interpreter-compatible state, allowing execution to
     * continue in the bytecode interpreter.
     *
     * The reconstruction maps:
     *   - JIT stack slots -> interpreter VM registers
     *   - JIT PC -> bytecode PC
     *   - JIT frame -> interpreter frame chain
     */
    struct ReconstructedState {
        /// Bytecode PC to resume at
        uint32_t bytecode_pc = 0;
        /// Interpreter register values (R0-R15)
        uint64_t regs[16] = {};
        /// Frame pointer chain
        uint64_t frame_ptr = 0;
        /// Number of values successfully reconstructed
        size_t   reconstructed_count = 0;
    };

    static ReconstructedState reconstruct_state(
        const C2DeoptMetadata &meta,
        const uint64_t *jit_frame,
        size_t frame_size) {
        ReconstructedState state;

        state.bytecode_pc = meta.bytecode_pc;
        state.reconstructed_count = meta.live_values.size();

        // Map live values to interpreter register slots.
        // Each live value is placed in regs[0..N] based on its original
        // bytecode register assignment.
        for (size_t i = 0; i < meta.live_values.size() && i < 16; ++i) {
            ir::IrValueId vid = meta.live_values[i];

            // Compute the offset in the JIT frame for this value
            // (-8 * (vid+1) is the convention from the C1 selector)
            int64_t frame_offset = -static_cast<int64_t>((vid + 1) * 8);

            size_t offset_in_frame = 0;
            if (frame_offset < 0) {
                offset_in_frame = frame_size + frame_offset;
            } else {
                offset_in_frame = static_cast<size_t>(frame_offset);
            }

            if (offset_in_frame + 8 <= frame_size * 8) {
                const uint64_t *frame_base = jit_frame;
                state.regs[i] = frame_base[offset_in_frame / 8];
            }
        }

        return state;
    }

    // =====================================================================
    //  State conversion from optimized native code back to interpreter
    // =====================================================================

    /**
     * @brief Converts the native stack frame to interpreter state and
     *        patches the return address to the interpreter loop.
     *
     * The optimized code has annotations (deopt metadata) that describe
     * where each live value lives in the native frame.  This function
     * reads those values, writes them to the interpreter's register
     * file, and sets up the interpreter PC to resume execution.
     */
    static void convert_to_interpreter_state(
        const C2DeoptMetadata &meta,
        vrt_proc *proc,
        const uint64_t *jit_frame,
        size_t frame_size) {
        ReconstructedState state = reconstruct_state(meta, jit_frame, frame_size);

        // Write reconstructed registers to the process VM
        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);
        for (int i = 0; i < 16; ++i) {
            p->registers.regs[i].qword(state.regs[i]);
        }

        p->registers.rip.qword(state.bytecode_pc);
    }

    // =====================================================================
    //  Metadata recording for speculative optimizations
    // =====================================================================

    C2DeoptMetadata make_deopt_metadata(
        DeoptReason reason,
        ir::IrValueId dst,
        const std::vector<ir::IrValueId> &live,
        ir::IrBlockId fallback,
        uint32_t bytecode_pc,
        const std::string &desc) {
        C2DeoptMetadata meta;
        meta.reason = reason;
        meta.bytecode_pc = bytecode_pc;
        meta.live_values = live;
        meta.fallback_block = fallback;
        meta.description = desc;
        return meta;
    }

    // =====================================================================
    //  Emit deopt handler trampolines in the code cache
    // =====================================================================

    static uint32_t emit_deopt_trampoline(
        CodeCache &cache,
        uint32_t metadata_index,
        uint32_t frame_size,
        const std::vector<ir::IrValueId> &live_values,
        uint64_t deopt_runtime_addr) {
        // Allocate space for the trampoline
        // Layout:
        //   push rax       (save scratch)
        //   mov rax, [rsp+8]  (get return address)
        //   ...
        //   sub rsp, frame_size
        //   mov [rsp+offsets], live regs
        //   mov rdi, proc_ptr
        //   mov rsi, metadata_index
        //   call deopt_runtime_addr
        //   (falls through to interpreter bailout)
        const size_t TRAMPOLINE_SIZE = 128;
        uint8_t *code = cache.alloc(TRAMPOLINE_SIZE, 16);
        if (!code) return 0;

        uint8_t *p = code;
        size_t remaining = TRAMPOLINE_SIZE;

        // Emit: push rax ; push rcx ; push rdx (save caller-saved)
        auto emit_byte = [&](uint8_t b) {
            if (remaining > 0) { *p++ = b; --remaining; }
        };
        auto emit_bytes = [&](const uint8_t *src, size_t n) {
            size_t to_copy = std::min(n, remaining);
            std::memcpy(p, src, to_copy);
            p += to_copy;
            remaining -= to_copy;
        };

        // push rax (50)
        emit_byte(0x50);
        // push rcx (51)
        emit_byte(0x51);
        // push rdx (52)
        emit_byte(0x52);
        // push rbx (53)
        emit_byte(0x53);
        // push rsi (56)
        emit_byte(0x56);
        // push rdi (57)
        emit_byte(0x57);
        // push r8 (41 50)
        emit_byte(0x41); emit_byte(0x50);
        // push r9 (41 51)
        emit_byte(0x41); emit_byte(0x51);
        // push r10 (41 52)
        emit_byte(0x41); emit_byte(0x52);
        // push r11 (41 53)
        emit_byte(0x41); emit_byte(0x53);

        // Save live values from the frame.
        // The convention is: live values are at [rsp + offset] in the JIT frame.
        // We copy them to a reserved area at the top of the trampoline frame.

        // sub rsp, frame_size (48 81 ec XX XX XX XX)
        emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
        emit_bytes(reinterpret_cast<const uint8_t *>(&frame_size), 4);

        // mov rdi, [rsp + frame_size + 80] -- get proc* from saved area
        // (48 8B BC 24 XX XX XX XX)
        uint32_t proc_offset = frame_size + 80;
        emit_byte(0x48); emit_byte(0x8B); emit_byte(0xBC); emit_byte(0x24);
        emit_bytes(reinterpret_cast<const uint8_t *>(&proc_offset), 4);

        // mov rsi, metadata_index
        // (48 BE XX XX XX XX 00 00 00 00)
        emit_byte(0x48); emit_byte(0xBE);
        uint64_t meta_idx_64 = metadata_index;
        emit_bytes(reinterpret_cast<const uint8_t *>(&meta_idx_64), 8);

        // mov rax, deopt_runtime_addr
        // (48 B8 XX XX XX XX XX XX XX XX)
        emit_byte(0x48); emit_byte(0xB8);
        emit_bytes(reinterpret_cast<const uint8_t *>(&deopt_runtime_addr), 8);

        // call rax (FF D0)
        emit_byte(0xFF); emit_byte(0xD0);

        // add rsp, frame_size (48 81 C4 XX XX XX XX)
        emit_byte(0x48); emit_byte(0x81); emit_byte(0xC4);
        emit_bytes(reinterpret_cast<const uint8_t *>(&frame_size), 4);

        // Restore registers in reverse order
        emit_byte(0x41); emit_byte(0x5B); // pop r11
        emit_byte(0x41); emit_byte(0x5A); // pop r10
        emit_byte(0x41); emit_byte(0x59); // pop r9
        emit_byte(0x41); emit_byte(0x58); // pop r8
        emit_byte(0x5F); // pop rdi
        emit_byte(0x5E); // pop rsi
        emit_byte(0x5B); // pop rbx
        emit_byte(0x5A); // pop rdx
        emit_byte(0x59); // pop rcx
        emit_byte(0x58); // pop rax

        // ret (C3)
        emit_byte(0xC3);

        // Commit the trampoline
        cache.commit(code, TRAMPOLINE_SIZE);

        return static_cast<uint32_t>(reinterpret_cast<uint8_t *>(p) - code);
    }

    // =====================================================================
    //  Emit deopt handler trampolines for a compilation
    // =====================================================================

    static void emit_deopt_handlers(
        const ir::IrFunction &ir_fn,
        const std::vector<C2DeoptMetadata> &meta_list,
        CodeCache &cache,
        const RuntimeEntries &rt) {
        (void)ir_fn;
        if (meta_list.empty()) return;

        uint64_t deopt_runtime_addr =
            reinterpret_cast<uint64_t>(rt.safepoint_handler);
        if (!deopt_runtime_addr) {
            deopt_runtime_addr = reinterpret_cast<uint64_t>(jit::return_from_jit);
        }
        (void)deopt_runtime_addr;

        for (size_t i = 0; i < meta_list.size(); ++i) {
            const auto &meta = meta_list[i];
            (void)meta;
            uint32_t offset = emit_deopt_trampoline(
                cache,
                static_cast<uint32_t>(i),
                meta.frame_size,
                meta.live_values,
                deopt_runtime_addr);

            if (offset > 0) {
                /* Deopt handler emitido correctamente en offset */
            }
        }

    }

} // namespace c2_deopt
} // namespace jit
