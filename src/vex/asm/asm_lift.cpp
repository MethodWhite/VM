/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_lift.cpp
 * @brief Implementacion del reconocimiento de patrones de asm liftables a IR.
 */

#include "vex/asm/asm_lift.h"

#include "vex/asm/asm_cfg.h"     // build_asm_cfg (estructura del bucle LL/SC arm64)
#include "vex/asm/asm_effects.h" // asm_canonical_reg

#include <cctype>
#include <string>
#include <vector>

namespace vex {

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string strip_comment(const std::string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ';')
            return s.substr(0, i);
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
            return s.substr(0, i);
    }
    return s;
}

/// ¿La linea es una etiqueta (identificador seguido de ':')?
bool is_label(const std::string &line) {
    const std::string t = trim(line);
    size_t colon = t.find(':');
    if (colon == std::string::npos)
        return false;
    for (size_t i = 0; i < colon; ++i)
        if (!(std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_' ||
              t[i] == '.'))
            return false;
    return colon > 0;
}

/// Instrucciones reales (sin comentarios/labels/vacias).
std::vector<std::string> real_insns(const std::string &body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string raw = body.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        std::string t = trim(strip_comment(raw));
        if (t.empty() || is_label(t))
            continue;
        out.push_back(t);
    }
    return out;
}

/// Trocea una instruccion en (mnemonico, [operandos]).  Respeta @c [..] (no
/// parte por comas dentro de corchetes).
void split_insn(const std::string &insn, std::string &mnem,
                std::vector<std::string> &ops) {
    mnem.clear();
    ops.clear();
    size_t i = 0;
    while (i < insn.size() && std::isspace(static_cast<unsigned char>(insn[i])))
        ++i;
    size_t j = i;
    while (j < insn.size() && !std::isspace(static_cast<unsigned char>(insn[j])))
        ++j;
    mnem = lower(insn.substr(i, j - i));
    // Operandos: partir por comas fuera de [..].
    std::string cur;
    int depth = 0;
    for (size_t k = j; k < insn.size(); ++k) {
        char c = insn[k];
        if (c == '[')
            ++depth;
        else if (c == ']')
            --depth;
        if (c == ',' && depth == 0) {
            std::string o = trim(cur);
            if (!o.empty())
                ops.push_back(o);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string o = trim(cur);
    if (!o.empty())
        ops.push_back(o);
}

/// Si @p op es una memoria @c [reg] PLANA (un solo registro, sin
/// desplazamiento/indice), devuelve el registro canonico; si no, "".
std::string plain_mem_reg(const std::string &op) {
    if (op.size() < 3 || op.front() != '[' || op.back() != ']')
        return "";
    std::string inner = trim(op.substr(1, op.size() - 2));
    // Rechazar cualquier operador de direccionamiento.
    for (char c : inner)
        if (c == '+' || c == '-' || c == '*' || c == ' ' || c == ':')
            return "";
    return asm_canonical_reg(inner);
}

/// Normaliza un `lock cmpxchg [addr], des`: devuelve true + addr/des canonicos.
bool parse_lock_cmpxchg(const std::string &insn, std::string &addr,
                        std::string &des) {
    std::string mnem;
    std::vector<std::string> ops;
    split_insn(insn, mnem, ops);
    if (mnem == "lock") {
        split_insn(trim(insn.substr(4)), mnem, ops);
    } else {
        return false; // sin lock -> no atomico cross-core
    }
    if (mnem != "cmpxchg" || ops.size() != 2) return false;
    addr = plain_mem_reg(ops[0]);
    if (addr.empty()) return false;
    const instr_db::ParsedOp d =
        instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
    if (d.kind != instr_db::OP_REG || d.width != 64) return false;
    des = asm_canonical_reg(ops[1]);
    return true;
}

/// ¿La instruccion @p insn ESCRIBE el registro canonico @p reg?  Consulta el
/// modelo de efectos completo (implicitos + operandos escritos) -> asi el lift
/// multi-instruccion verifica que nada intermedio pise el valor esperado.
bool insn_writes_reg(const std::string &insn, const std::string &reg) {
    std::string mnem;
    std::vector<std::string> ops;
    split_insn(insn, mnem, ops);
    if (mnem == "lock" && !ops.empty()) split_insn(trim(insn.substr(4)), mnem, ops);
    const AsmEffects eff = asm_effects_for(mnem, "x86_64");
    for (const std::string &w : eff.implicit_write)
        if (w == reg) return true;
    for (size_t i = 0; i < ops.size(); ++i)
        if ((eff.operand_write_mask >> i) & 1u)
            if (asm_canonical_reg(ops[i]) == reg) return true;
    return !eff.known; // desconocida -> conservador (puede escribir)
}

/// Lift multi-instruccion x86: `mov rax, <exp>` (setup del expected) seguido de
/// `lock cmpxchg [addr], <des>`, con expected EXPLICITO.  Verifica (efectos)
/// que ninguna instruccion intermedia escriba rax.
AsmLift lift_x86_cas_multi(const std::vector<std::string> &insns) {
    AsmLift r;
    // La ULTIMA instruccion debe ser el lock cmpxchg.
    std::string addr, des;
    const size_t last = insns.size() - 1;
    if (!parse_lock_cmpxchg(insns[last], addr, des)) {
        r.note = "multi: la ultima instruccion no es lock cmpxchg [reg],reg64";
        return r;
    }
    // Buscar hacia atras el `mov rax, <exp>` que fija el esperado.
    for (size_t i = last; i-- > 0;) {
        std::string mnem;
        std::vector<std::string> ops;
        split_insn(insns[i], mnem, ops);
        if (mnem == "mov" && ops.size() == 2 &&
            asm_canonical_reg(ops[0]) == "rax") {
            const instr_db::ParsedOp e =
                instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
            if (e.kind != instr_db::OP_REG || e.width != 64) {
                r.note = "multi: el expected (mov rax, X) debe ser un reg64";
                return r;
            }
            // Verificar que nada ENTRE el mov y el cmpxchg pise rax.
            for (size_t j = i + 1; j < last; ++j)
                if (insn_writes_reg(insns[j], "rax")) {
                    r.note = "multi: una instruccion intermedia pisa rax";
                    return r;
                }
            r.op = AsmLiftOp::AtomicCas;
            r.addr_reg = addr;
            r.exp_reg = asm_canonical_reg(ops[1]); // EXPLICITO
            r.des_reg = des;
            r.result_reg = "rax"; // old value queda en rax
            r.width = 64;
            return r;
        }
        // Si una instruccion intermedia pisa rax ANTES de hallar el mov, no hay
        // setup limpio del expected.
        if (insn_writes_reg(insns[i], "rax")) {
            r.note = "multi: rax se escribe sin un `mov rax, <exp>` claro";
            return r;
        }
    }
    r.note = "multi: no se hallo el setup `mov rax, <exp>` del expected";
    return r;
}

AsmLift lift_x86(const std::vector<std::string> &insns) {
    AsmLift r;
    if (insns.empty()) {
        r.note = "bloque vacio";
        return r;
    }
    // Multi-instruccion: setup del expected + lock cmpxchg.
    if (insns.size() != 1) return lift_x86_cas_multi(insns);

    std::string mnem;
    std::vector<std::string> ops;
    split_insn(insns[0], mnem, ops);

    // Requiere el prefijo `lock` (atomicidad cross-core).  split_insn lo trata
    // como mnemonico si va pegado; lo normalizamos: si mnem=="lock", el
    // verdadero mnemonico es el primer operando-token... mejor detectarlo aqui.
    bool has_lock = false;
    if (mnem == "lock") {
        has_lock = true;
        // Re-trocear quitando el "lock ".
        const std::string rest = trim(insns[0].substr(4));
        split_insn(rest, mnem, ops);
    }
    if (!has_lock) {
        r.note = "cmpxchg/xadd sin prefijo lock no es atomico cross-core";
        return r;
    }

    if (mnem == "cmpxchg") {
        // cmpxchg [addr], desired ; expected/old implicitos en rax.
        if (ops.size() != 2) {
            r.note = "cmpxchg: se esperan 2 operandos";
            return r;
        }
        const std::string addr = plain_mem_reg(ops[0]);
        if (addr.empty()) {
            r.note = "cmpxchg: el 1er operando debe ser [reg] plano";
            return r;
        }
        instr_db::ParsedOp des = instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
        if (des.kind != instr_db::OP_REG || des.width != 64) {
            r.note = "cmpxchg: solo i64 (registro de 64 bits) soportado";
            return r;
        }
        r.op = AsmLiftOp::AtomicCas;
        r.addr_reg = addr;
        r.des_reg = asm_canonical_reg(ops[1]);
        r.exp_reg = "rax";    // expected implicito
        r.result_reg = "rax"; // old value queda en rax
        r.width = 64;
        return r;
    }

    if (mnem == "xadd") {
        // xadd [addr], src ; src recibe el valor viejo (fetch-and-add).
        if (ops.size() != 2) {
            r.note = "xadd: se esperan 2 operandos";
            return r;
        }
        const std::string addr = plain_mem_reg(ops[0]);
        if (addr.empty()) {
            r.note = "xadd: el 1er operando debe ser [reg] plano";
            return r;
        }
        instr_db::ParsedOp src = instr_db::parse_operand(instr_db::Isa::X86, ops[1]);
        if (src.kind != instr_db::OP_REG || src.width != 64) {
            r.note = "xadd: solo i64 (registro de 64 bits) soportado";
            return r;
        }
        r.op = AsmLiftOp::AtomicAdd;
        r.addr_reg = addr;
        r.des_reg = asm_canonical_reg(ops[1]); // delta
        r.result_reg = r.des_reg;              // old value queda en el mismo reg
        r.width = 64;
        return r;
    }

    r.note = "mnemonico no liftible (" + mnem + ")";
    return r;
}

// --- arm64: bucle load-linked / store-conditional ---

/// Canonicaliza un registro arm64 a su fisico de 64 bits: x0/w0 -> "x0".  El
/// registro de estado del stlxr (w) y el objeto de 64 bits (x) del MISMO numero
/// comparten fisico -> mismo canonico (permite cruzar el `w` del cbnz con el del
/// stlxr).
std::string arm_canon(const std::string &raw) {
    const std::string s = lower(trim(raw));
    if (s.size() < 2 || (s[0] != 'x' && s[0] != 'w'))
        return "";
    for (size_t i = 1; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return "";
    return "x" + s.substr(1);
}

/// ¿El registro arm es de 64 bits (prefijo x)?
bool arm_is_x64(const std::string &raw) {
    const std::string s = lower(trim(raw));
    return !s.empty() && s[0] == 'x';
}

/// Registro de una memoria arm @c [reg] plana (sin offset/indice), canonico.
std::string arm_mem_reg(const std::string &op) {
    if (op.size() < 3 || op.front() != '[' || op.back() != ']')
        return "";
    const std::string inner = trim(op.substr(1, op.size() - 2));
    for (char c : inner)
        if (c == '+' || c == '-' || c == '*' || c == ' ' || c == ',')
            return "";
    return arm_canon(inner);
}

/// Reconoce el bucle CAS load-linked/store-conditional de arm64.  Verifica el
/// EFECTO de TODAS las instrucciones del bloque (no solo un patron laxo): cada
/// una debe ser exactamente su rol esperado y los saltos deben formar el bucle.
/// Cualquier instruccion o arista inesperada -> no se lifta.
///   L_retry: ldaxr <old>, [<addr>]
///            cmp   <old>, <exp>
///            b.ne  <L_exit>
///            stlxr <flag>, <des>, [<addr>]
///            cbnz  <flag>, <L_retry>
AsmLift lift_arm64(const std::string &body) {
    AsmLift r;
    const AsmCfg cfg = build_asm_cfg(instr_db::Isa::ARM64, body);
    const auto &in = cfg.insns;
    if (in.size() != 5) {
        r.note = "arm64: solo el bucle LL/SC canonico de 5 instrucciones";
        return r;
    }

    std::string m;
    std::vector<std::string> o;

    // 0) L_retry: ldaxr/ldxr old, [addr]  (debe tener etiqueta = destino del bucle)
    if (in[0].labels.empty()) {
        r.note = "arm64: el ldaxr debe llevar la etiqueta del bucle (retry)";
        return r;
    }
    split_insn(in[0].text, m, o);
    if ((m != "ldaxr" && m != "ldxr") || o.size() != 2) {
        r.note = "arm64: se esperaba ldaxr <old>, [<addr>]";
        return r;
    }
    const std::string old = arm_canon(o[0]);
    if (old.empty() || !arm_is_x64(o[0])) {
        r.note = "arm64: solo i64 (registro x de 64 bits)";
        return r;
    }
    const std::string addr = arm_mem_reg(o[1]);
    if (addr.empty()) {
        r.note = "arm64: ldaxr requiere [reg] plano";
        return r;
    }

    // 1) cmp old, exp
    split_insn(in[1].text, m, o);
    if (m != "cmp" || o.size() != 2 || arm_canon(o[0]) != old) {
        r.note = "arm64: se esperaba cmp <old>, <exp>";
        return r;
    }
    const std::string exp = arm_canon(o[1]);
    if (exp.empty()) {
        r.note = "arm64: el 2o operando del cmp no es un registro";
        return r;
    }

    // 2) b.ne L_exit
    split_insn(in[2].text, m, o);
    if (m != "b.ne" || in[2].term != AsmTerm::CondBranch) {
        r.note = "arm64: se esperaba b.ne <exit>";
        return r;
    }
    const std::string exit_lbl = in[2].target;

    // 3) stlxr/stxr flag, des, [addr]
    split_insn(in[3].text, m, o);
    if ((m != "stlxr" && m != "stxr") || o.size() != 3) {
        r.note = "arm64: se esperaba stlxr <flag>, <des>, [<addr>]";
        return r;
    }
    const std::string flag = arm_canon(o[0]);
    const std::string des = arm_canon(o[1]);
    if (des.empty() || !arm_is_x64(o[1])) {
        r.note = "arm64: <des> solo i64 (registro x)";
        return r;
    }
    if (arm_mem_reg(o[2]) != addr) {
        r.note = "arm64: la direccion del stlxr no coincide con la del ldaxr";
        return r;
    }

    // 4) cbnz flag, L_retry
    split_insn(in[4].text, m, o);
    if (m != "cbnz" || o.size() != 2 || in[4].term != AsmTerm::CondBranch) {
        r.note = "arm64: se esperaba cbnz <flag>, <retry>";
        return r;
    }
    if (arm_canon(o[0]) != flag) {
        r.note = "arm64: el registro de estado del cbnz no coincide con el stlxr";
        return r;
    }
    const std::string retry_lbl = in[4].target;
    bool retry_ok = false;
    for (const auto &l : in[0].labels)
        if (l == retry_lbl)
            retry_ok = true;
    if (!retry_ok) {
        r.note = "arm64: el cbnz no vuelve al ldaxr (no es el bucle LL/SC)";
        return r;
    }
    if (exit_lbl == retry_lbl) {
        r.note = "arm64: b.ne y cbnz apuntan al mismo destino";
        return r;
    }

    // Todas las instrucciones y aristas verificadas: es un compare-and-swap.
    r.op = AsmLiftOp::AtomicCas;
    r.addr_reg = addr;
    r.exp_reg = exp;
    r.des_reg = des;
    r.result_reg = old;
    r.width = 64;
    return r;
}

} // namespace

AsmLift asm_lift_detect(instr_db::Isa isa, const std::string &body) {
    switch (isa) {
    case instr_db::Isa::X86: {
        std::vector<std::string> insns = real_insns(body);
        if (insns.empty()) {
            AsmLift r;
            r.note = "bloque vacio";
            return r;
        }
        return lift_x86(insns);
    }
    case instr_db::Isa::ARM64:
        return lift_arm64(body);
    default:
        break;
    }
    AsmLift r;
    r.note = "lift no soportado para esta ISA todavia";
    return r;
}

} // namespace vex
