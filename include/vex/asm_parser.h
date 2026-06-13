#ifndef VEX_ASM_PARSER_H
#define VEX_ASM_PARSER_H

#include <cstdint>
#include <string>
#include <vector>

#include "vex/diagnostic.h"

namespace vex {

enum class AsmConstraintKind : uint8_t {
    REGISTER,       ///< "r" - any general-purpose register
    MEMORY,         ///< "m" - memory reference
    IMMEDIATE,      ///< "i" - compile-time constant
    REG_SPECIFIC,   ///< "a", "b", "c", "d", "S", "D" - specific register
    OUTPUT_REG,     ///< "=r" - output via register
    OUTPUT_MEM,     ///< "=m" - output to memory
    OUTPUT_REG_SPEC,///< "=a", "=b", "=c", "=d", "=S", "=D"
    READWRITE_REG,  ///< "+r" - read-write register
    READWRITE_SPEC, ///< "+a", "+b", "+c", "+d"
    COUNT
};

struct AsmOperand {
    AsmConstraintKind kind = AsmConstraintKind::REGISTER;
    std::string       constraint_text;  ///< raw constraint string like "=r", "r", "m"
    std::string       var_name;         ///< variable name from the asm expression
    bool              is_output  = false;
    bool              is_input   = false;
    bool              is_readwrite = false;
    uint8_t           operand_index = 0; ///< position in the operand list
    /// For REG_SPECIFIC / OUTPUT_REG_SPEC / READWRITE_SPEC: the specific register
    std::string       specific_register;
};

struct AsmClobber {
    std::string name;  ///< "memory", "cc", or register name like "rax", "r12"
};

struct AsmParseResult {
    std::string              assembly_text;    ///< template with %0..%N placeholders
    std::vector<AsmOperand>  operands;
    std::vector<AsmClobber>  clobbers;
    bool                     is_volatile = false;
    bool                     has_output  = false;
    bool                     has_input   = false;

    /// Returns the resolved assembly string with register names substituted
    /// for each operand placeholder %N.
    std::string resolve(const std::vector<std::string> &register_assignments) const;
};

class AsmParser {
public:
    explicit AsmParser(Diagnostics &diags);

    AsmParseResult parse(const std::string &full_asm_text, SourceLoc loc);

    /// Validate a clobber name against known register names.
    static bool is_valid_clobber(const std::string &name);

    /// Map a constraint like "=r", "=a", "+r", "r", "m", "i" to its kind.
    static AsmConstraintKind classify_constraint(const std::string &c);

    /// Get the specific register name for a constraint like "a", "b", "c", "d", "S", "D".
    static std::string specific_register_name(AsmConstraintKind kind);

    /// Known register names for clobber validation.
    static const std::vector<std::string> &known_registers();

private:
    Diagnostics &diags_;
};

} // namespace vex

#endif // VEX_ASM_PARSER_H
