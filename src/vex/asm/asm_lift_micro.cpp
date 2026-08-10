/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/asm/asm_lift_micro.cpp
 * @brief Lift de instrucciones asm opacas SIN operandos de registro a
 *        @c IrOp::ASM_MICRO.  Ver vx/asm/asm_lift_micro.h.
 */

#include "vex/asm/asm_lift_micro.h"

#include "ir/ssa_ir.h"
#include "vex/asm/asm_effects.h"
#include "vex/asm/asm_phys_reg.h"

#include <cctype>
#include <string>
#include <vector>

namespace vex {

namespace {

/// Recorta espacios de los extremos.
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

/// Divide el cuerpo en instrucciones (una por linea), descartando comentarios
/// (@c // y @c ;) y etiquetas (@c "name:").
std::vector<std::string> instructions(const std::string &body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string ln = body.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        // Quitar comentario // o ;
        size_t c = ln.find("//");
        if (c != std::string::npos) ln.resize(c);
        c = ln.find(';');
        if (c != std::string::npos) ln.resize(c);
        ln = trim(ln);
        if (!ln.empty() && ln.back() != ':') // no etiqueta
            out.push_back(ln);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

/// Empaqueta los efectos de la DB en el byte @c eff de @ref ir::AsmMicro:
/// bit0 memoria, bit1 lee flags, bit2 escribe flags, bit3 barrera.
uint8_t pack_eff(const instr_db::AsmInsnSem &sem) {
    uint8_t e = 0;
    if (sem.reads_mem || sem.writes_mem) e |= 0x1;
    if (sem.reads_flags) e |= 0x2;
    if (sem.writes_flags) e |= 0x4;
    if (sem.barrier) e |= 0x8;
    return e;
}

/// Minusculas de @p s.
std::string lower(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (char)std::tolower((unsigned char)c);
    return o;
}

/// ¿Contiene @p v (canonico, minusculas) el nombre @p name?
bool has_reg(const std::vector<std::string> &v, const std::string &name) {
    for (const std::string &r : v)
        if (lower(r) == name) return true;
    return false;
}

/// Trocea @p insn en (mnemonico, operandos por coma).  Los operandos van con
/// espacios recortados.  No maneja @c [...] (= solo registros GP).
void split_insn(const std::string &insn, std::string &mnem,
                std::vector<std::string> &ops) {
    ops.clear();
    size_t sp = insn.find_first_of(" \t");
    if (sp == std::string::npos) { // sin operandos
        mnem = insn;
        return;
    }
    mnem = insn.substr(0, sp);
    std::string rest = trim(insn.substr(sp + 1));
    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t comma = rest.find(',', pos);
        std::string tok = rest.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        tok = trim(tok);
        if (!tok.empty()) ops.push_back(tok);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

/// construye la lista PLANA de operandos de FiSICO FIJO (solo GP) y la
/// plantilla con @c $N a partir de la linea + la semantica de la DB.  Devuelve
/// @c false si algun operando NO es un registro GP nombrable (MEM/IMM/FP/VEC ->
///) -> el llamador emite @c INLINE_ASM.
bool build_operands(
    const std::string &insn, const instr_db::AsmInsnSem &sem,
    const std::unordered_map<std::string, ir::IrValueId> &slot_of,
    std::vector<ir::AsmMicroOperand> &operands, std::string &tmpl) {
    operands.clear();
    std::string mnem;
    std::vector<std::string> toks;
    split_insn(insn, mnem, toks);
    if (toks.empty()) return false; // sin operandos textuales -> path sin-ops

    tmpl = mnem;
    for (size_t k = 0; k < toks.size(); ++k) {
        uint16_t w = 0;
        const int phys = vex::asm_x86_gp_index(toks[k], &w);
        if (phys < 0) return false; // no es GP (MEM/IMM/FP/VEC)
        ir::AsmMicroOperand op;
        op.kind = ir::AsmOperandKind::REG;
        op.regclass = vex::ASM_RC_GP;
        op.width = w;
        op.fixed_phys = (int16_t)phys; // fisico fijo del texto (constraint RA)
        // Un registro LIGADO a una variable Vesta (register()) necesita
        // threading SSA + el asignador (constraint en el RA + marshalling en
        // los backends).  Hoy esos bloques caen al
        // INLINE_ASM, que ya resuelve los bindings en interp/JIT/AOT.
        if (slot_of.find(lower(toks[k])) != slot_of.end()) return false;
        op.value = ir::IR_NO_VALUE; // fisico opaco (sin SSA)
        // Rol del operando = presencia de su reg canonico en reads/writes DB.
        const std::string cn = lower(toks[k]);
        uint8_t fl = 0;
        if (has_reg(sem.reads, cn)) fl |= ir::ASM_OP_READ;
        if (has_reg(sem.writes, cn)) fl |= ir::ASM_OP_WRITE;
        if (fl == 0) return false; // operando sin rol conocido -> conservador
        op.flags = fl;
        operands.push_back(op);
        tmpl += (k == 0 ? " $" : ", $") + std::to_string(k);
    }
    return true;
}

} // namespace

bool asm_lift_micro(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body, uint32_t line,
    const std::unordered_map<std::string, ir::IrValueId> &slot_of) {
    const std::vector<std::string> insns = instructions(body);
    if (insns.empty()) return false;

    // Microarq para la semantica: solo usamos los campos SEMaNTICOS (form_id,
    // barrera, mem, flags, reads/writes), no la latencia, asi que cualquier
    // microarq valida de la ISA sirve.  Skylake para x86; fallback 0.
    int32_t ua = instr_db::microarch_by_name(isa, "intel-skylake");
    if (ua < 0) ua = 0;

    // Fase 1 (validacion transaccional): TODAS las instrucciones deben ser
    // formas conocidas por la DB, y O BIEN sin operandos de registro (mfence,
    // cpuid, ...), O BIEN con operandos de FiSICO FIJO GP (: popcnt/tzcnt/
    // bswap rax,...).  Cualquier otra cosa (MEM/IMM/FP/VEC/implicitos,)
    // -> false y el llamador emite INLINE_ASM.
    std::vector<instr_db::AsmInsnSem> sems;
    std::vector<std::vector<ir::AsmMicroOperand>> ops_per;
    std::vector<std::string> tmpl_per;
    sems.reserve(insns.size());
    ops_per.reserve(insns.size());
    tmpl_per.reserve(insns.size());
    // ASA: arch string para consultar los efectos IMPLICITOS (asm_effects).  La
    // DB de instrucciones (instr_db) modela la SEMANTICA de la forma (operandos
    // explicitos), pero NO los registros que una instruccion lee/escribe por
    // convencion de ABI: `syscall` lee el numero en RAX y los args en
    // RDI/RSI/RDX/R10/R8/R9; `int` lee EAX+args; `svc` lee X8+X0..X7.
    const std::string arch_s =
        (isa == instr_db::Isa::ARM64) ? "arm64" : "x86_64";
    for (const std::string &insn : insns) {
        instr_db::AsmInsnSem sem =
            instr_db::asm_insn_sem(isa, insn, (uint32_t)ua);
        if (sem.form_id < 0)          // desconocida por la DB
            return false;
        // Si la instruccion LEE/ESCRIBE implicitamente un registro que esta
        // LIGADO a una variable Vesta (register(): p.ej. `syscall` con
        // register("rax") id + register("rdi") a1 en los params del invoke), el
        // asm_micro NO puede modelar esos operandos implicitos (solo trocea los
        // textuales) -> los args no se threadean, el RA no los coloca y el DCE
        // elimina sus stores.  Lo dejamos al INLINE_ASM, que SI marca los
        // bindings como in/out vregs y respeta el pin al registro fisico.
        {
            std::string mnem;
            size_t sp = insn.find_first_of(" \t");
            mnem = (sp == std::string::npos) ? insn : insn.substr(0, sp);
            vex::AsmEffects ef = vex::asm_effects_for(mnem, arch_s);
            bool binds_implicit = false;
            for (const std::string &r : ef.implicit_read)
                if (slot_of.find(lower(r)) != slot_of.end()) {
                    binds_implicit = true;
                    break;
                }
            if (!binds_implicit)
                for (const std::string &r : ef.implicit_write)
                    if (slot_of.find(lower(r)) != slot_of.end()) {
                        binds_implicit = true;
                        break;
                    }
            if (binds_implicit) return false; // -> INLINE_ASM con bindings
        }
        std::vector<ir::AsmMicroOperand> operands;
        std::string tmpl;
        if (sem.reads.empty() && sem.writes.empty()) {
            // Sin operandos de registro: plantilla verbatim (mfence/lfence/...).
            tmpl = insn;
        } else if (!build_operands(insn, sem, slot_of, operands, tmpl)) {
            return false;             // operandos no soportados
        }
        sems.push_back(std::move(sem));
        ops_per.push_back(std::move(operands));
        tmpl_per.push_back(std::move(tmpl));
    }

    // Fase 2 (emision): una ASM_MICRO por instruccion.
    for (size_t i = 0; i < insns.size(); ++i) {
        ir::AsmMicro am;
        am.isa = (uint8_t)isa;
        am.form_id = (uint32_t)sems[i].form_id;
        am.tmpl = std::move(tmpl_per[i]);
        am.operands = std::move(ops_per[i]);
        am.eff = pack_eff(sems[i]);

        ir::IrInstr in{};
        in.op = ir::IrOp::ASM_MICRO;
        in.type = ir::IrType::VOID;
        in.dst = ir::IR_NO_VALUE;
        in.imm = fn.asm_micros.size();
        in.source_line = line;
        fn.asm_micros.push_back(std::move(am));
        fn.append(block, std::move(in));
    }
    return true;
}

} // namespace vex
