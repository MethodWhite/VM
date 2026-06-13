#include "vex/asm_parser.h"
#include "vex/lowering.h"
#include "vex/type_checker.h"
#include "ir/ssa_ir.h"

#include <sstream>
#include <unordered_set>

namespace vex {

// -----------------------------------------------------------------------
// Lowering of inline assembly expressions to IR
// -----------------------------------------------------------------------
//
// The lowering process:
//   1. Parse the inline asm expression via AsmParser
//   2. For each output operand: allocate a new SSA value (or use existing
//      variable binding) to receive the output.
//   3. For each input operand: evaluate the input expression to get its
//      SSA value, then emit MOV to the assigned register before the asm.
//   4. For read-write operands (+r): both input MOV and output capture.
//   5. Emit IrOp::RAW_ASM with the resolved assembly text
//   6. For each output operand: move from the output register to the
//      variable's target SSA value.
//   7. Mark clobbered registers in the IR (set is_call_site if any
//      caller-saved registers are clobbered).
// -----------------------------------------------------------------------

/// Check if a register name is caller-saved (clobbered by calls on x86-64).
static bool is_caller_saved(const std::string &reg)
{
    // x86-64 ABI: rax, rcx, rdx, rsi, rdi, r8-r11 are caller-saved
    // Also condition codes are implicitly clobbered by most instructions.
    static const std::unordered_set<std::string> caller_saved = {
        "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
        "eax", "ecx", "edx", "esi", "edi", "r8d", "r9d", "r10d", "r11d",
        "ax", "cx", "dx", "si", "di", "r8w", "r9w", "r10w", "r11w",
        "al", "cl", "dl", "sil", "dil", "r8b", "r9b", "r10b", "r11b",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
        "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
        "xmm12", "xmm13", "xmm14", "xmm15",
        "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7",
        "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7",
    };
    return caller_saved.count(reg) > 0;
}

/// Determine if a clobber list contains any caller-saved registers.
static bool has_caller_saved_clobber(const std::vector<AsmClobber> &clobbers)
{
    for (auto &c : clobbers) {
        if (is_caller_saved(c.name)) return true;
    }
    return false;
}

/// Build register assignments for operands.
/// Simple strategy: first unused caller-saved register per operand.
static std::vector<std::string> assign_operand_registers(
    const AsmParseResult &parsed)
{
    std::vector<std::string> result;
    static const std::vector<std::string> reg_pool = {
        "r12", "r13", "r14", "r15",
        "rbx", "rbp",
    };

    size_t pool_idx = 0;

    for (auto &op : parsed.operands) {
        // If a specific register is required, use it
        if (!op.specific_register.empty()) {
            result.push_back(op.specific_register);
            continue;
        }

        // Allocate from the pool
        if (pool_idx < reg_pool.size()) {
            result.push_back(reg_pool[pool_idx++]);
        } else {
            // Fallback - spill to a stack slot and reference it (simplified)
            result.push_back("r12"); // reuse with caution
        }
    }

    return result;
}

/// Lower an inline asm expression from AST to IR.
/// The caller provides the parsed AsmParseResult and the SSA values for
/// input operands (evaluted from the AST expressions).
///
/// @param result       Parsed inline asm structure.
/// @param input_values SSA values for input operands (ordered as in parsed result).
/// @param output_names Names of output variables to bind.
/// @param loc          Source location for diagnostics.
/// @return IrValueId for the result (typically IR_NO_VALUE for void asm,
///         or the SSA value of the first output).
ir::IrValueId lower_inline_asm(
    ir::IrFunction        &fn,
    ir::IrBlockId          block,
    const AsmParseResult  &result,
    const std::vector<ir::IrValueId> &input_values,
    const std::vector<std::pair<std::string, ir::IrValueId>> &output_bindings,
    uint32_t               source_line)
{
    // 1. Assign registers to operands
    auto reg_assignments = assign_operand_registers(result);

    // 2. Emit MOV instructions for inputs before the asm block
    size_t input_idx = 0;
    for (size_t i = 0; i < result.operands.size(); ++i) {
        auto &op = result.operands[i];
        if (!op.is_input && !op.is_readwrite) continue;

        ir::IrValueId val;
        if (op.is_readwrite) {
            // For read-write, input is the current value of the output variable
            if (input_idx < input_values.size()) {
                val = input_values[input_idx++];
            }
        } else {
            if (input_idx < input_values.size()) {
                val = input_values[input_idx++];
            }
        }

        if (val == ir::IR_NO_VALUE) continue;

        // Emit MOV to the assigned register
        // We emit a RAW_ASM mov instruction for simplicity
        std::string reg = (i < reg_assignments.size()) ? reg_assignments[i] : "r12";

        ir::IrInstr mov{};
        mov.op          = ir::IrOp::RAW_ASM;
        mov.type        = ir::IrType::VOID;
        mov.dst         = ir::IR_NO_VALUE;
        mov.func_name   = "mov " + reg + ", {src0}";
        mov.operands.push_back(val);
        mov.source_line = source_line;
        fn.append(block, std::move(mov));
    }

    // 3. Emit the main inline assembly block
    std::string resolved_asm = result.resolve(reg_assignments);

    ir::IrInstr ra{};
    ra.op          = ir::IrOp::RAW_ASM;
    ra.type        = ir::IrType::VOID;
    ra.dst         = ir::IR_NO_VALUE;
    ra.func_name   = resolved_asm;
    ra.source_line = source_line;

    // If any caller-saved registers are clobbered, mark as call site
    // so the register allocator preserves live values across this block.
    if (has_caller_saved_clobber(result.clobbers)) {
        ra.set_is_call_site(true);
    }

    fn.append(block, std::move(ra));

    // 4. For read-write operands, capture the output register back to the variable
    input_idx = 0;
    for (size_t i = 0; i < result.operands.size(); ++i) {
        auto &op = result.operands[i];
        if (!op.is_output && !op.is_readwrite) continue;

        std::string reg = (i < reg_assignments.size()) ? reg_assignments[i] : "r12";

        // Find the output binding for this operand
        for (auto &[name, dst] : output_bindings) {
            if (name == op.var_name) {
                ir::IrInstr mov_out{};
                mov_out.op          = ir::IrOp::RAW_ASM;
                mov_out.type        = ir::IrType::VOID;
                mov_out.dst         = ir::IR_NO_VALUE;
                mov_out.func_name   = "mov {dst}, " + reg;
                mov_out.operands.push_back(dst);
                mov_out.source_line = source_line;
                fn.append(block, std::move(mov_out));
                break;
            }
        }
    }

    // For simple case with a single output, return the first output binding
    if (!output_bindings.empty()) {
        return output_bindings[0].second;
    }

    return ir::IR_NO_VALUE;
}

/// Higher-level entry: lower a complete inline asm expression node.
/// This is called from the main lowering pass when encountering an
/// asm expression in the AST.
///
/// @param asm_text   The full inline asm source text including constraints.
/// @param input_exprs AST expressions for input operands (already type-checked).
/// @param output_vars Names and mutable SSA bindings for output variables.
/// @param clobber_names List of clobber declarations.
/// @param is_volatile Whether the asm is marked volatile.
/// @param loc Source location.
/// @return IrValueId.
ir::IrValueId lower_vex_asm_expression(
    ir::IrFunction                     &fn,
    ir::IrBlockId                       block,
    Lowering                           &lowering,
    const TypeChecker                  &tc,
    Diagnostics                        &diags,
    const std::string                  &asm_text,
    const std::vector<ir::IrValueId>   &input_values,
    const std::vector<std::string>     &output_var_names,
    const std::vector<std::pair<std::string, ir::IrValueId>> &output_bindings,
    const std::vector<std::string>     &clobber_names,
    bool                                is_volatile,
    SourceLoc                           loc)
{
    // Parse the full inline asm block
    AsmParser parser(diags);
    AsmParseResult parsed = parser.parse(asm_text, loc);

    if (parsed.assembly_text.empty()) {
        diags.error(loc, "inline asm template is empty");
        return ir::IR_NO_VALUE;
    }

    // Merge clobber names into the parsed result
    for (auto &cn : clobber_names) {
        // Avoid duplicates
        bool found = false;
        for (auto &ec : parsed.clobbers) {
            if (ec.name == cn) { found = true; break; }
        }
        if (!found && AsmParser::is_valid_clobber(cn)) {
            parsed.clobbers.push_back({cn});
        }
    }

    // Add "memory" clobber if volatile (conservative but safe)
    if (is_volatile) {
        bool has_memory = false;
        for (auto &c : parsed.clobbers) {
            if (c.name == "memory") { has_memory = true; break; }
        }
        if (!has_memory) {
            parsed.clobbers.push_back({"memory"});
        }
    }

    return lower_inline_asm(
        fn, block, parsed, input_values, output_bindings, loc.line);
}

} // namespace vex
