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
 * @file asm_cfg.cpp
 * @brief Implementacion de la reconstruccion del CFG de un bloque de inline asm.
 */

#include "vex/asm/asm_cfg.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace vex {

namespace {

/// ¿Es @p c parte de un identificador de asm (letra, digito, @c _ / @c . / @c $)?
inline bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
           c == '$';
}

/// Minusculiza una cadena ASCII.
std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Recorta espacios/tabs en ambos extremos.
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

/// Quita el comentario de linea (@c ; estilo NASM o @c // estilo C).
std::string strip_comment(const std::string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ';')
            return s.substr(0, i);
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
            return s.substr(0, i);
    }
    return s;
}

/// Extrae el primer token (mnemonico), en minusculas.
std::string first_token(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    size_t j = i;
    while (j < line.size() && ident_char(line[j]))
        ++j;
    return lower(line.substr(i, j - i));
}

/// Devuelve el resto de la linea tras el mnemonico (los operandos, trim).
std::string operand_str(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    size_t j = i;
    while (j < line.size() && ident_char(line[j]))
        ++j;
    return trim(line.substr(j));
}

/// Primer operando (hasta la primera coma), trim.
std::string first_operand(const std::string &ops) {
    size_t c = ops.find(',');
    return trim(c == std::string::npos ? ops : ops.substr(0, c));
}

/// Ultimo operando (tras la ultima coma), trim.
std::string last_operand(const std::string &ops) {
    size_t c = ops.rfind(',');
    return trim(c == std::string::npos ? ops : ops.substr(c + 1));
}

/// ¿El operando es una ETIQUETA (identificador simple, no registro/mem/inmediato)?
/// Un identificador que NO es un registro real de la ISA (via @ref parse_operand,
/// que conoce los bancos por-ISA) y no es @c [..]/@c (..)/@c #.. ni numero.
bool looks_like_label(instr_db::Isa isa, const std::string &op) {
    if (op.empty())
        return false;
    if (op[0] == '[' || op[0] == '(' || op[0] == '#' || op[0] == '$')
        return false; // memoria x86/riscv, inmediato arm/at&t
    if (std::isdigit(static_cast<unsigned char>(op[0])))
        return false; // inmediato/offset numerico
    for (char c : op)
        if (!ident_char(c))
            return false; // contiene espacios/operadores -> no es etiqueta simple
    // Descartar registros reales (rax/x0/a1/...) y memoria: son operandos, no
    // destinos de salto.  parse_operand da width>0 solo a registros reconocidos.
    instr_db::ParsedOp p = instr_db::parse_operand(isa, op);
    if (p.kind == instr_db::OP_MEM)
        return false;
    if (p.kind == instr_db::OP_REG && p.width > 0)
        return false;
    return true;
}

// --- Clasificacion de terminador por ISA ---

AsmTerm classify_x86(instr_db::Isa isa, const std::string &mn,
                     const std::string &ops, std::string &target) {
    if (mn == "ret" || mn == "retn" || mn == "retf" || mn == "iret" ||
        mn == "iretd" || mn == "iretq" || mn == "sysret" || mn == "sysexit" ||
        mn == "hlt" || mn == "ud2")
        return AsmTerm::Ret;
    if (mn == "call" || mn == "syscall" || mn == "sysenter" || mn == "int" ||
        mn == "int3") {
        // Una llamada retorna: fallthrough.  (El destino del callee no es parte
        // del CFG del bloque.)
        return AsmTerm::Call;
    }
    if (mn == "jmp") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::UncondJump;
        }
        return AsmTerm::Indirect; // jmp reg/[mem]
    }
    // Condicionales: jCC, loop*, jcxz/jecxz/jrcxz.
    if (!mn.empty() && mn[0] == 'j' && mn != "jmp") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::CondBranch;
        }
        return AsmTerm::Indirect;
    }
    if (mn == "loop" || mn == "loope" || mn == "loopne" || mn == "loopz" ||
        mn == "loopnz") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::CondBranch;
        }
        return AsmTerm::Indirect;
    }
    return AsmTerm::Fallthrough;
}

AsmTerm classify_arm64(instr_db::Isa isa, const std::string &mn,
                       const std::string &ops, std::string &target) {
    if (mn == "ret")
        return AsmTerm::Ret;
    if (mn == "bl") {
        // bl label -> llamada (retorna).  blr reg -> llamada indirecta.
        std::string op = first_operand(ops);
        return AsmTerm::Call;
    }
    if (mn == "blr")
        return AsmTerm::Call;
    if (mn == "br")
        return AsmTerm::Indirect; // br reg
    if (mn == "b") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::UncondJump;
        }
        return AsmTerm::Indirect;
    }
    // b.CC condicional.
    if (mn.size() > 2 && mn[0] == 'b' && mn[1] == '.') {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::CondBranch;
        }
    }
    // cbz/cbnz reg,label ; tbz/tbnz reg,#imm,label -> destino = ultimo operando.
    if (mn == "cbz" || mn == "cbnz" || mn == "tbz" || mn == "tbnz") {
        std::string op = last_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::CondBranch;
        }
    }
    return AsmTerm::Fallthrough;
}

AsmTerm classify_arm32(instr_db::Isa isa, const std::string &mn,
                       const std::string &ops, std::string &target) {
    if (mn == "bx" || mn == "bxj") {
        // bx lr = retorno; bx reg = indirecto.
        std::string op = lower(first_operand(ops));
        return (op == "lr" || op == "r14") ? AsmTerm::Ret : AsmTerm::Indirect;
    }
    if (mn == "pop") {
        // pop {..., pc} restaura PC -> retorno.
        if (lower(ops).find("pc") != std::string::npos)
            return AsmTerm::Ret;
        return AsmTerm::Fallthrough;
    }
    // bl / blx = llamada (con o sin sufijo de condicion).
    if (mn == "bl" || mn == "blx" || mn.rfind("bl", 0) == 0) {
        std::string op = first_operand(ops);
        if (mn.size() >= 2 && (mn[0] == 'b') && (mn[1] == 'l'))
            return AsmTerm::Call;
    }
    // b / bCC (beq, bne, bge, ...): b incondicional; con sufijo de condicion, rama.
    if (mn == "b") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::UncondJump;
        }
        return AsmTerm::Indirect;
    }
    static const char *kCC[] = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl",
                                "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"};
    if (mn.size() == 3 && mn[0] == 'b') {
        std::string cc = mn.substr(1);
        for (const char *c : kCC)
            if (cc == c) {
                std::string op = first_operand(ops);
                if (looks_like_label(isa, op)) {
                    target = op;
                    return AsmTerm::CondBranch;
                }
            }
    }
    return AsmTerm::Fallthrough;
}

AsmTerm classify_riscv(instr_db::Isa isa, const std::string &mn,
                       const std::string &ops, std::string &target) {
    if (mn == "ret")
        return AsmTerm::Ret;
    if (mn == "jr")
        return AsmTerm::Indirect; // jr reg
    if (mn == "call" || mn == "jal") {
        // jal rd, label (o pseudo jal label) -> llamada si guarda ra; para el CFG
        // del bloque, retorna -> fallthrough.
        return AsmTerm::Call;
    }
    if (mn == "jalr")
        return AsmTerm::Indirect;
    if (mn == "j" || mn == "tail") {
        std::string op = first_operand(ops);
        if (looks_like_label(isa, op)) {
            target = op;
            return AsmTerm::UncondJump;
        }
        return AsmTerm::Indirect;
    }
    // beq/bne/blt/bge/bltu/bgeu rs1,rs2,label ; beqz/bnez rs,label.
    static const char *kB[] = {"beq",  "bne",  "blt",  "bge", "bltu",
                               "bgeu", "beqz", "bnez", "blez", "bgez",
                               "bltz", "bgtz"};
    for (const char *b : kB)
        if (mn == b) {
            std::string op = last_operand(ops);
            if (looks_like_label(isa, op)) {
                target = op;
                return AsmTerm::CondBranch;
            }
        }
    return AsmTerm::Fallthrough;
}

} // namespace

AsmTerm asm_classify_term(instr_db::Isa isa, const std::string &line,
                          std::string &target) {
    target.clear();
    const std::string body = strip_comment(line);
    const std::string mn = first_token(body);
    if (mn.empty())
        return AsmTerm::Fallthrough;
    const std::string ops = operand_str(body);
    switch (isa) {
    case instr_db::Isa::X86:
        return classify_x86(isa, mn, ops, target);
    case instr_db::Isa::ARM64:
        return classify_arm64(isa, mn, ops, target);
    case instr_db::Isa::ARM32:
        return classify_arm32(isa, mn, ops, target);
    case instr_db::Isa::RISCV:
        return classify_riscv(isa, mn, ops, target);
    }
    return AsmTerm::Fallthrough;
}

AsmCfg build_asm_cfg(instr_db::Isa isa, const std::string &body) {
    AsmCfg cfg;

    // --- 1) Trocear en instrucciones, recolectando las etiquetas por delante. ---
    // Una linea puede llevar varias etiquetas y/o una instruccion: "L1: L2: add".
    std::vector<std::string> pending_labels;
    uint32_t line_no = 0;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string raw =
            body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        ++line_no;

        std::string cur = trim(strip_comment(raw));
        // Consumir etiquetas al inicio: "name:" (posiblemente varias, y con una
        // instruccion detras en la misma linea).
        while (!cur.empty()) {
            size_t colon = cur.find(':');
            if (colon == std::string::npos)
                break;
            std::string cand = trim(cur.substr(0, colon));
            // Etiqueta valida: identificador simple, sin espacios.
            bool ok = !cand.empty();
            for (char c : cand)
                if (!ident_char(c))
                    ok = false;
            // Evitar confundir memoria/expresiones con dos puntos (raro en asm).
            if (!ok)
                break;
            pending_labels.push_back(cand);
            cur = trim(cur.substr(colon + 1));
        }
        if (cur.empty())
            continue; // linea solo-etiqueta(s) o vacia -> las labels quedan pending.

        AsmInsn in;
        in.text = cur;
        in.labels = pending_labels;
        in.line_no = line_no;
        in.term = asm_classify_term(isa, cur, in.target);
        pending_labels.clear();
        if (in.term == AsmTerm::Indirect)
            cfg.has_indirect = true;
        if (in.term == AsmTerm::Unknown)
            cfg.unknown_terminators.push_back(first_token(cur));
        cfg.insns.push_back(std::move(in));
    }

    // Nodo de SALIDA sintetico (`nop`).  El bloque asm sale implicitamente al
    // FINAL: por fall-through de la ultima instruccion, por la rama NO-tomada de
    // un jCC final, o via una etiqueta de salida al final sin instruccion detras
    // (idioma `... jCC .end; ...; .end:`).  Sin un bloque real que represente ese
    // punto, (a) la rama no-tomada de un jCC final se queda SIN sucesor de
    // fall-through -> el lift ve succs!=2 y falla; (b) las etiquetas finales se
    // pierden -> saltos sin resolver (falsos VXA002/VXA003).  Lo anclamos aqui
    // (uniforme para lift/diagnosticos/diagramas/efectos).  NO se anade tras un
    // terminador incondicional (jmp/ret) sin etiqueta final: ahi no hay salida
    // por fall-through (un `jmp .top` final ES un bucle infinito -> VXA003 real).
    bool need_exit = !pending_labels.empty();
    if (!need_exit && !cfg.insns.empty()) {
        const AsmTerm t = cfg.insns.back().term;
        need_exit = (t == AsmTerm::Fallthrough || t == AsmTerm::CondBranch);
    }
    if (need_exit) {
        AsmInsn in;
        in.text = "nop";
        in.labels = pending_labels;
        in.line_no = line_no;
        in.term = asm_classify_term(isa, in.text, in.target);
        pending_labels.clear();
        cfg.insns.push_back(std::move(in));
    }

    if (cfg.insns.empty())
        return cfg;

    // Mapa etiqueta -> indice de instruccion donde se define.
    std::unordered_map<std::string, uint32_t> label_at;
    for (uint32_t i = 0; i < cfg.insns.size(); ++i)
        for (const std::string &l : cfg.insns[i].labels)
            label_at[l] = i;

    // --- 2) Lideres = primera instr, destino de salto, e instr tras un salto. ---
    std::vector<bool> leader(cfg.insns.size(), false);
    leader[0] = true;
    for (uint32_t i = 0; i < cfg.insns.size(); ++i) {
        const AsmInsn &in = cfg.insns[i];
        // Una instruccion con etiqueta puede ser destino -> lider.
        if (!in.labels.empty())
            leader[i] = true;
        if (in.term == AsmTerm::UncondJump || in.term == AsmTerm::CondBranch) {
            auto it = label_at.find(in.target);
            if (it != label_at.end())
                leader[it->second] = true;
            else
                cfg.has_unresolved_target = true;
        }
        // La instruccion siguiente a cualquier salto/rama/ret es lider.
        if (in.term != AsmTerm::Fallthrough && in.term != AsmTerm::Call &&
            i + 1 < cfg.insns.size())
            leader[i + 1] = true;
        // Tras una CALL tambien empieza bloque nuevo (util para diagramas), pero
        // NO rompe el flujo (fallthrough).  Lo dejamos en el mismo bloque para
        // no fragmentar de mas.
    }

    // --- 3) Formar bloques basicos a partir de los lideres. ---
    std::vector<uint32_t> block_of(cfg.insns.size(), 0);
    for (uint32_t i = 0; i < cfg.insns.size(); ++i) {
        if (leader[i]) {
            AsmBasicBlock bb;
            bb.first = i;
            bb.last = i;
            bb.label = cfg.insns[i].labels.empty() ? std::string()
                                                    : cfg.insns[i].labels.front();
            cfg.blocks.push_back(bb);
        }
        uint32_t bidx = static_cast<uint32_t>(cfg.blocks.size() - 1);
        block_of[i] = bidx;
        cfg.blocks[bidx].last = i;
        cfg.blocks[bidx].term = cfg.insns[i].term;
    }

    // --- 4) Aristas segun el terminador del ultimo insn de cada bloque. ---
    auto add_edge = [&](uint32_t from, uint32_t to) {
        auto &s = cfg.blocks[from].succs;
        if (std::find(s.begin(), s.end(), to) == s.end())
            s.push_back(to);
        auto &p = cfg.blocks[to].preds;
        if (std::find(p.begin(), p.end(), from) == p.end())
            p.push_back(from);
    };
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        const AsmInsn &term_in = cfg.insns[cfg.blocks[b].last];
        const uint32_t next_insn = cfg.blocks[b].last + 1;
        const bool has_next = next_insn < cfg.insns.size();
        switch (term_in.term) {
        case AsmTerm::UncondJump: {
            auto it = label_at.find(term_in.target);
            if (it != label_at.end())
                add_edge(b, block_of[it->second]);
            break; // sin fallthrough.
        }
        case AsmTerm::CondBranch: {
            auto it = label_at.find(term_in.target);
            if (it != label_at.end())
                add_edge(b, block_of[it->second]);
            if (has_next)
                add_edge(b, block_of[next_insn]); // fallthrough.
            break;
        }
        case AsmTerm::Ret:
            break; // sin sucesores.
        case AsmTerm::Indirect:
            // Destino desconocido: fallthrough conservador si lo hay (el CFG queda
            // marcado impreciso).
            if (has_next)
                add_edge(b, block_of[next_insn]);
            break;
        case AsmTerm::Fallthrough:
        case AsmTerm::Call:
        case AsmTerm::Unknown:
        default:
            if (has_next)
                add_edge(b, block_of[next_insn]);
            break;
        }
    }

    return cfg;
}

} // namespace vex
