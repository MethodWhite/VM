#include "vex/asm_parser.h"
#include "vex/type_checker.h"
#include "vex/diagnostic.h"

#include <cctype>
#include <unordered_set>

namespace vex {

// -----------------------------------------------------------------------
// Type checking for inline assembly
// -----------------------------------------------------------------------
//
// Rules:
//   - "r" (register)  -> any integer type that fits in a register (<= u64)
//   - "m" (memory)    -> reference/pointer type
//   - "i" (immediate) -> compile-time constant integer
//   - "=r" (output)   -> mutable variable (lvalue)
//   - "+r" (readwrite)-> mutable variable (lvalue), must be initialized
//   - "=a", "=b", etc -> specific register output, mutable variable
//   - "a", "b", etc   -> specific register input, must fit in register
//
//   For each output operand, check that the variable is mutable (not const).
//   For volatile asm, verify it has at least one side-effect (output or
//   memory clobber) -- warn if dead.
//   Warn about missing clobber declarations for registers modified by
//   the instruction (e.g., CPUID clobbers eax, ebx, ecx, edx).
// -----------------------------------------------------------------------

/// Size in bytes of a Vex type when used as an assembly operand.
static size_t asm_operand_size(const Type &t)
{
    if (t.kind == PrimitiveKind::PTR) return 8;
    return primitive_size_bytes(t.kind);
}

/// Check if a constraint string expects a specific register.
static bool is_specific_register_constraint(const std::string &c)
{
    std::string clean = c;
    if (!clean.empty() && (clean[0] == '=' || clean[0] == '+'))
        clean = clean.substr(1);
    return clean == "a" || clean == "b" || clean == "c" || clean == "d" ||
           clean == "S" || clean == "D";
}

/// Get register name from a constraint like "=a" -> "rax", "+d" -> "rdx"
static std::string constraint_to_register(const std::string &c)
{
    std::string clean = c;
    if (!clean.empty() && (clean[0] == '=' || clean[0] == '+'))
        clean = clean.substr(1);
    if (clean == "a") return "rax";
    if (clean == "b") return "rbx";
    if (clean == "c") return "rcx";
    if (clean == "d") return "rdx";
    if (clean == "S") return "rsi";
    if (clean == "D") return "rdi";
    return "";
}

/// CPUID is known to clobber eax, ebx, ecx, edx regardless of declared clobbers.
static const std::unordered_set<std::string> s_known_asm_effects = {
    "cpuid", "rdtsc", "rdtscp", "xgetbv", "xsetbv"
};

/// Returns the set of registers implicitly clobbered by a known instruction.
static std::vector<std::string> implicit_clobbers(const std::string &asm_text)
{
    // Normalize: lowercase and find the mnemonic
    std::string lower;
    for (char ch : asm_text) lower.push_back(static_cast<char>(std::tolower(ch)));

    // Check for known instructions
    if (lower.find("cpuid") != std::string::npos) {
        return {"rax", "rbx", "rcx", "rdx"};
    }
    if (lower.find("rdtsc") != std::string::npos) {
        return {"rax", "rdx"};
    }
    if (lower.find("xgetbv") != std::string::npos) {
        return {"rax", "rdx"};
    }
    if (lower.find("xsetbv") != std::string::npos) {
        return {"rdx", "rax"};
    }

    return {};
}

// -----------------------------------------------------------------------
// Main entry point
// -----------------------------------------------------------------------

/// Check an inline asm block for type correctness.
/// This is invoked from the type checker when processing an asm expression.
///
/// @param asm_text     Full inline asm text (template + constraints + clobbers).
/// @param input_types  Types of input expressions (in order).
/// @param output_types Types of output variables (in order).
/// @param is_volatile  Whether the asm is marked volatile.
/// @param loc          Source location.
/// @param tc           TypeChecker instance (for diagnostics etc).
/// @return true if the asm block is well-typed.
bool check_inline_asm(
    const std::string                        &asm_text,
    const std::vector<Type>                  &input_types,
    const std::vector<std::pair<std::string, Type>> &output_vars,
    bool                                      is_volatile,
    SourceLoc                                 loc,
    TypeChecker                              &tc,
    Diagnostics                              &diags)
{
    AsmParser parser(diags);
    AsmParseResult parsed = parser.parse(asm_text, loc);

    if (parsed.assembly_text.empty()) {
        // Error already reported by parser
        return false;
    }

    bool ok = true;

    // Collect output operands from parsed result
    std::vector<AsmOperand> output_ops;
    std::vector<AsmOperand> input_ops;
    std::vector<AsmOperand> rw_ops; // read-write

    for (auto &op : parsed.operands) {
        if (op.is_readwrite) {
            rw_ops.push_back(op);
        } else if (op.is_output) {
            output_ops.push_back(op);
        } else if (op.is_input) {
            input_ops.push_back(op);
        }
    }

    // Check output variable types against constraints
    size_t out_idx = 0;
    for (auto &op : output_ops) {
        if (out_idx >= output_vars.size()) {
            diags.error(loc, "more output operands in asm than provided variables");
            ok = false;
            break;
        }

        const Type &var_type = output_vars[out_idx].second;
        size_t var_size = asm_operand_size(var_type);

        // Check constraint compatibility with type
        switch (op.kind) {
            case AsmConstraintKind::OUTPUT_REG:
            case AsmConstraintKind::OUTPUT_REG_SPEC: {
                // Must be a register-sized type (<= 8 bytes) or pointer
                if (var_size > 8) {
                    diags.error(loc,
                        "output operand '" + op.constraint_text +
                        "' requires a type <= 8 bytes, got " +
                        type_to_string(var_type) + " (" +
                        std::to_string(var_size) + " bytes)");
                    ok = false;
                }
                break;
            }
            case AsmConstraintKind::OUTPUT_MEM: {
                // Memory constraint accepts any type that has an address
                // (reference or pointer type, or mutable variable)
                if (var_type.kind != PrimitiveKind::PTR &&
                    var_type.kind != PrimitiveKind::CLASS) {
                    // Also accept any variable type (it has a stack address)
                }
                break;
            }
            default:
                break;
        }

        // Check that the variable is mutable
        // The type checker's symbol table should have is_const=false.
        // We look up the variable via the type checker.
        const Symbol *sym = tc.lookup(op.var_name);
        if (sym && sym->is_const) {
            diags.error(loc,
                "output operand '" + op.var_name + "' must be a mutable variable, "
                "but '" + op.var_name + "' is const");
            ok = false;
        }

        ++out_idx;
    }

    // Check input operand types against constraints
    size_t in_idx = 0;
    for (auto &op : input_ops) {
        if (in_idx >= input_types.size()) {
            diags.error(loc, "more input operands in asm than provided expressions");
            ok = false;
            break;
        }

        const Type &expr_type = input_types[in_idx];
        size_t expr_size = asm_operand_size(expr_type);

        switch (op.kind) {
            case AsmConstraintKind::REGISTER:
            case AsmConstraintKind::REG_SPECIFIC: {
                if (expr_size > 8) {
                    diags.error(loc,
                        "input operand '" + op.constraint_text +
                        "' requires a type <= 8 bytes, got " +
                        type_to_string(expr_type) + " (" +
                        std::to_string(expr_size) + " bytes)");
                    ok = false;
                }
                break;
            }
            case AsmConstraintKind::MEMORY: {
                if (expr_type.kind != PrimitiveKind::PTR &&
                    expr_type.kind != PrimitiveKind::CLASS) {
                    diags.error(loc,
                        "input operand with 'm' constraint requires a pointer "
                        "or reference type, got " + type_to_string(expr_type));
                    ok = false;
                }
                break;
            }
            case AsmConstraintKind::IMMEDIATE: {
                if (!is_integral(expr_type.kind)) {
                    diags.error(loc,
                        "input operand with 'i' constraint requires an "
                        "integer constant, got " + type_to_string(expr_type));
                    ok = false;
                }
                break;
            }
            default:
                break;
        }

        ++in_idx;
    }

    // Check read-write operands
    for (auto &op : rw_ops) {
        // Read-write must be a mutable variable (checked above for const)
        const Symbol *sym = tc.lookup(op.var_name);
        if (sym && sym->is_const) {
            diags.error(loc,
                "read-write operand '" + op.var_name +
                "' must be a mutable variable");
            ok = false;
        }
    }

    // Check that output variable names match actual variable names
    for (auto &op : output_ops) {
        if (!tc.lookup(op.var_name)) {
            diags.error(loc,
                "undefined variable '" + op.var_name +
                "' used as asm output operand");
            ok = false;
        }
    }

    // Check that input variable names exist
    for (auto &op : input_ops) {
        if (!op.var_name.empty() && !tc.lookup(op.var_name)) {
            diags.error(loc,
                "undefined variable '" + op.var_name +
                "' used as asm input operand");
            ok = false;
        }
    }

    // ---- Warnings ----

    // Warn if a known instruction is used but clobbers are missing
    auto implicit = implicit_clobbers(parsed.assembly_text);
    if (!implicit.empty()) {
        for (auto &reg : implicit) {
            bool declared = false;
            for (auto &c : parsed.clobbers) {
                if (c.name == reg || c.name == reg.substr(1)) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                // Check if the register is used as an explicit operand
                bool used_as_operand = false;
                for (auto &op : parsed.operands) {
                    if (op.specific_register == reg) {
                        used_as_operand = true;
                        break;
                    }
                }
                if (!used_as_operand) {
                    diags.warning(loc,
                        "inline asm uses '" + parsed.assembly_text +
                        "' which implicitly clobbers " + reg +
                        "; consider adding it to the clobber list");
                }
            }
        }
    }

    // Warn about volatile asm with no outputs and no memory clobber
    if (is_volatile && parsed.operands.empty()) {
        bool has_memory = false;
        for (auto &c : parsed.clobbers) {
            if (c.name == "memory") { has_memory = true; break; }
        }
        if (!has_memory) {
            diags.warning(loc,
                "volatile inline asm with no outputs and no \"memory\" clobber "
                "may be optimized away");
        }
    }

    // Warn if variable is used as input but not declared as clobbered
    // when the instruction modifies it (e.g., cpuid with eax as input
    // but eax not declared as clobbered - cpuid always writes eax).
    if (!parsed.assembly_text.empty()) {
        std::string lower;
        for (char ch : parsed.assembly_text) lower.push_back(static_cast<char>(std::tolower(ch)));
        bool is_modifying_inst = (lower.find("cpuid") != std::string::npos ||
                                  lower.find("rdtsc") != std::string::npos);
        if (is_modifying_inst) {
            for (auto &op : parsed.operands) {
                if (op.is_input && !op.is_output && !op.is_readwrite) {
                    std::string reg = constraint_to_register(op.constraint_text);
                    if (!reg.empty()) {
                        bool clobbered = false;
                        for (auto &c : parsed.clobbers) {
                            if (c.name == reg) { clobbered = true; break; }
                        }
                        if (!clobbered) {
                            diags.warning(loc,
                                "input operand '" + op.var_name +
                                "' uses register " + reg +
                                " which is clobbered by " + parsed.assembly_text +
                                "; declare it in the clobber list");
                        }
                    }
                }
            }
        }
    }

    return ok;
}

} // namespace vex
