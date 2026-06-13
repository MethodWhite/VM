#include "vex/asm_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace vex {

// -----------------------------------------------------------------------
// Known x86-64 register names (general purpose + condition codes clobber)
// -----------------------------------------------------------------------
static const std::vector<std::string> s_known_registers = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
    "ax",  "bx",  "cx",  "dx",  "si",  "di",  "bp",  "sp",
    "al",  "bl",  "cl",  "dl",  "sil", "dil", "bpl", "spl",
    "r8b",  "r9b",  "r10b",  "r11b",  "r12b",  "r13b",  "r14b", "r15b",
    "r8w",  "r9w",  "r10w",  "r11w",  "r12w",  "r13w",  "r14w", "r15w",
    "r8d",  "r9d",  "r10d",  "r11d",  "r12d",  "r13d",  "r14d", "r15d",
    "xmm0",  "xmm1",  "xmm2",  "xmm3",  "xmm4",  "xmm5",
    "xmm6",  "xmm7",  "xmm8",  "xmm9",  "xmm10", "xmm11",
    "xmm12", "xmm13", "xmm14", "xmm15",
    "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7",
    "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7",
    // condition codes
    "cc",
    // special
    "memory"
};

// -----------------------------------------------------------------------
// AsmParseResult::resolve
// -----------------------------------------------------------------------
std::string AsmParseResult::resolve(
    const std::vector<std::string> &register_assignments) const
{
    std::string result = assembly_text;
    for (size_t i = 0; i < operands.size() && i < register_assignments.size(); ++i) {
        std::string placeholder = "%" + std::to_string(i);
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.length(), register_assignments[i]);
            pos += register_assignments[i].length();
        }
    }
    return result;
}

// -----------------------------------------------------------------------
// AsmParser
// -----------------------------------------------------------------------
AsmParser::AsmParser(Diagnostics &diags)
    : diags_(diags)
{
}

// -----------------------------------------------------------------------
// Constraint classification
// -----------------------------------------------------------------------
AsmConstraintKind AsmParser::classify_constraint(const std::string &c)
{
    if (c.empty()) return AsmConstraintKind::REGISTER;

    // Read-write: "+r", "+a", "+b", "+c", "+d"
    if (c[0] == '+') {
        std::string rest = c.substr(1);
        if (rest == "r")  return AsmConstraintKind::READWRITE_REG;
        if (rest == "a")  return AsmConstraintKind::READWRITE_SPEC;
        if (rest == "b")  return AsmConstraintKind::READWRITE_SPEC;
        if (rest == "c")  return AsmConstraintKind::READWRITE_SPEC;
        if (rest == "d")  return AsmConstraintKind::READWRITE_SPEC;
        return AsmConstraintKind::READWRITE_REG; // fallback
    }

    // Output: "=r", "=m", "=a", "=b", "=c", "=d", "=S", "=D"
    if (c[0] == '=') {
        std::string rest = c.substr(1);
        if (rest == "r")  return AsmConstraintKind::OUTPUT_REG;
        if (rest == "m")  return AsmConstraintKind::OUTPUT_MEM;
        if (rest == "a" || rest == "b" || rest == "c" || rest == "d" ||
            rest == "S" || rest == "D") {
            return AsmConstraintKind::OUTPUT_REG_SPEC;
        }
        return AsmConstraintKind::OUTPUT_REG;
    }

    // Input constraints:
    if (c == "r")   return AsmConstraintKind::REGISTER;
    if (c == "m")   return AsmConstraintKind::MEMORY;
    if (c == "i")   return AsmConstraintKind::IMMEDIATE;
    if (c == "a" || c == "b" || c == "c" || c == "d" ||
        c == "S" || c == "D") {
        return AsmConstraintKind::REG_SPECIFIC;
    }

    return AsmConstraintKind::REGISTER;
}

std::string AsmParser::specific_register_name(AsmConstraintKind kind)
{
    switch (kind) {
        case AsmConstraintKind::REG_SPECIFIC:
        case AsmConstraintKind::OUTPUT_REG_SPEC:
        case AsmConstraintKind::READWRITE_SPEC:
            return "";
        default:
            return "";
    }
}

// -----------------------------------------------------------------------
// Clobber validation
// -----------------------------------------------------------------------
bool AsmParser::is_valid_clobber(const std::string &name)
{
    auto &regs = known_registers();
    return std::find(regs.begin(), regs.end(), name) != regs.end();
}

const std::vector<std::string> &AsmParser::known_registers()
{
    return s_known_registers;
}

// -----------------------------------------------------------------------
// Main parse function
// -----------------------------------------------------------------------
// Expected format (GCC-style inline asm):
//   "template" : outputs : inputs : clobbers
//   "template" : outputs : inputs
//   "template" : outputs
//   "template"
//
// Within outputs/inputs:
//   "constraint"(variable_name)
//   "constraint"(variable_name), ...
//
// Clobbers:
//   "name1", "name2", ...
//
// Optional "volatile" keyword after "asm":
//   asm volatile ( "..." : ... : ... : ... )
// -----------------------------------------------------------------------
AsmParseResult AsmParser::parse(const std::string &full_asm_text, SourceLoc loc)
{
    AsmParseResult result;

    // Full text looks like: `"mov %0, %1" : "=r"(out) : "r"(in) : "rax","memory"`
    // We need to split on unquoted colons.  Approach: parse character-by-character
    // tracking quoted strings.

    enum class Section {
        TEMPLATE,
        OUTPUTS,
        INPUTS,
        CLOBBERS
    };

    Section section = Section::TEMPLATE;

    // Split into components:
    // component 0: template string (including quotes)
    // component 1: outputs section
    // component 2: inputs section
    // component 3: clobbers section
    std::vector<std::string> sections;
    std::string current;
    int depth_paren = 0;
    bool in_string = false;

    for (size_t i = 0; i < full_asm_text.size(); ++i) {
        char ch = full_asm_text[i];

        if (ch == '"' && (i == 0 || full_asm_text[i-1] != '\\')) {
            in_string = !in_string;
            current += ch;
            continue;
        }

        if (in_string) {
            current += ch;
            continue;
        }

        // Track parentheses depth to avoid splitting on ':' inside constraints
        if (ch == '(') {
            depth_paren++;
            current += ch;
            continue;
        }
        if (ch == ')') {
            depth_paren--;
            current += ch;
            continue;
        }

        // Colon separator - but only at top level (not inside parens)
        if (ch == ':' && depth_paren == 0) {
            sections.push_back(current);
            current.clear();
            // Skip whitespace after colon
            while (i + 1 < full_asm_text.size() &&
                   (full_asm_text[i+1] == ' ' || full_asm_text[i+1] == '\t')) {
                ++i;
            }
            continue;
        }

        current += ch;
    }
    // Push the last section
    if (!current.empty() || sections.size() > 0) {
        sections.push_back(current);
    }

    // Minimum: template string
    if (sections.empty()) {
        diags_.error(loc, "empty inline assembly block");
        return result;
    }

    // Parse template: strip surrounding quotes
    std::string raw_template = sections[0];
    // Remove leading/trailing whitespace and quotes
    {
        size_t first = raw_template.find_first_of('"');
        size_t last  = raw_template.find_last_of('"');
        if (first != std::string::npos && last != std::string::npos && last > first) {
            result.assembly_text = raw_template.substr(first + 1, last - first - 1);
        } else {
            result.assembly_text = raw_template;
        }
    }

    // Helper to parse a constraint section: `"constraint"(var_name), ...`
    auto parse_constraint_section = [&](const std::string &sec,
                                         bool is_output_section) {
        std::vector<AsmOperand> ops;
        std::string s = sec;
        // Strip outer whitespace
        {
            size_t st = s.find_first_not_of(" \t");
            if (st != std::string::npos) s = s.substr(st);
            size_t en = s.find_last_not_of(" \t");
            if (en != std::string::npos) s = s.substr(0, en + 1);
        }
        if (s.empty()) return ops;

        // Parse comma-separated items: "constraint"(var_name)
        size_t pos = 0;
        while (pos < s.size()) {
            // Skip whitespace
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            if (pos >= s.size()) break;

            // Find opening quote for constraint
            if (s[pos] != '"') {
                diags_.error(loc, "expected constraint string in inline asm");
                break;
            }
            size_t q1 = pos;
            size_t q2 = s.find('"', q1 + 1);
            if (q2 == std::string::npos) {
                diags_.error(loc, "unterminated constraint string in inline asm");
                break;
            }
            std::string constraint = s.substr(q1 + 1, q2 - q1 - 1);
            pos = q2 + 1;

            // Skip whitespace
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;

            // Expect '('
            if (pos >= s.size() || s[pos] != '(') {
                diags_.error(loc, "expected '(' after constraint in inline asm");
                break;
            }
            ++pos; // skip '('

            // Find variable name
            size_t vstart = pos;
            while (pos < s.size() && s[pos] != ')') ++pos;
            std::string var_name = s.substr(vstart, pos - vstart);
            // Trim whitespace from var_name
            {
                size_t vs = var_name.find_first_not_of(" \t");
                size_t ve = var_name.find_last_not_of(" \t");
                if (vs != std::string::npos && ve != std::string::npos)
                    var_name = var_name.substr(vs, ve - vs + 1);
            }

            if (pos >= s.size() || s[pos] != ')') {
                diags_.error(loc, "expected ')' after variable name in inline asm");
                break;
            }
            ++pos; // skip ')'

            AsmOperand op;
            op.constraint_text = constraint;
            op.var_name        = var_name;
            op.kind            = classify_constraint(constraint);

            // Set is_output / is_input based on constraint prefix and section
            if (!constraint.empty() && constraint[0] == '=') {
                op.is_output = true;
            } else if (!constraint.empty() && constraint[0] == '+') {
                op.is_output  = true;
                op.is_input   = true;
                op.is_readwrite = true;
            } else {
                op.is_input = true;
            }

            // For specific register constraints
            if (op.kind == AsmConstraintKind::REG_SPECIFIC ||
                op.kind == AsmConstraintKind::OUTPUT_REG_SPEC ||
                op.kind == AsmConstraintKind::READWRITE_SPEC) {
                std::string rest = constraint;
                if (!rest.empty() && (rest[0] == '=' || rest[0] == '+'))
                    rest = rest.substr(1);
                if (rest == "a") op.specific_register = "rax";
                else if (rest == "b") op.specific_register = "rbx";
                else if (rest == "c") op.specific_register = "rcx";
                else if (rest == "d") op.specific_register = "rdx";
                else if (rest == "S") op.specific_register = "rsi";
                else if (rest == "D") op.specific_register = "rdi";
            }

            ops.push_back(std::move(op));

            // Skip whitespace and optional comma
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            }
        }
        return ops;
    };

    // Parse outputs (section 1)
    if (sections.size() > 1) {
        result.operands = parse_constraint_section(sections[1], true);
        for (auto &op : result.operands) {
            op.is_output = true;
        }
        result.has_output = !result.operands.empty();
    }

    // Parse inputs (section 2)
    if (sections.size() > 2) {
        auto inputs = parse_constraint_section(sections[2], false);
        // Append inputs after outputs
        for (auto &op : inputs) {
            op.is_input = true;
            op.operand_index = static_cast<uint8_t>(result.operands.size());
        }
        result.operands.insert(result.operands.end(),
                                std::make_move_iterator(inputs.begin()),
                                std::make_move_iterator(inputs.end()));
        result.has_input = !inputs.empty();
    }

    // Parse clobbers (section 3)
    if (sections.size() > 3) {
        std::string clobber_section = sections[3];
        // Strip whitespace and parse comma-separated quoted strings
        size_t pos = 0;
        while (pos < clobber_section.size()) {
            // Skip whitespace
            while (pos < clobber_section.size() &&
                   (clobber_section[pos] == ' ' || clobber_section[pos] == '\t'))
                ++pos;
            if (pos >= clobber_section.size()) break;

            if (clobber_section[pos] != '"') {
                diags_.error(loc, "expected quoted clobber name in inline asm");
                break;
            }
            size_t q1 = pos;
            size_t q2 = clobber_section.find('"', q1 + 1);
            if (q2 == std::string::npos) {
                diags_.error(loc, "unterminated clobber string in inline asm");
                break;
            }
            std::string cname = clobber_section.substr(q1 + 1, q2 - q1 - 1);
            pos = q2 + 1;

            if (!is_valid_clobber(cname)) {
                diags_.error(loc, "invalid clobber '" + cname + "' in inline asm");
            } else {
                result.clobbers.push_back({cname});
            }

            // Skip whitespace and comma
            while (pos < clobber_section.size() &&
                   (clobber_section[pos] == ' ' || clobber_section[pos] == '\t'))
                ++pos;
            if (pos < clobber_section.size() && clobber_section[pos] == ',') {
                ++pos;
            }
        }
    }

    // Detect "volatile" - check if the raw_template starts with a
    // "volatile" keyword before the quoted string
    {
        std::string trimmed = sections[0];
        size_t st = trimmed.find_first_not_of(" \t");
        if (st != std::string::npos) {
            if (trimmed.substr(st, 8) == "volatile" || trimmed.substr(st, 8) == "VOLATILE") {
                result.is_volatile = true;
            }
        }
    }

    // Re-number operand indices
    for (size_t i = 0; i < result.operands.size(); ++i) {
        result.operands[i].operand_index = static_cast<uint8_t>(i);
    }

    return result;
}

} // namespace vex
