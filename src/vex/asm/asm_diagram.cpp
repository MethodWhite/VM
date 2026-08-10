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
 * @file asm_diagram.cpp
 * @brief Implementacion de los diagramas del CFG de asm anotados con coste.
 */

#include "vex/asm/asm_diagram.h"

#include "vex/asm/asm_cfg.h"
#include "vex/asm/asm_diag.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace vex {

namespace {

/// Formatea un float con 1 decimal (para latencia/throughput).
std::string f1(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

/// Puerto mas cargado (cuello de botella) de un AsmBlockCost: (nombre, uops).
std::pair<std::string, float>
bottleneck(const instr_db::AsmBlockCost &c) {
    std::pair<std::string, float> best{"", 0.0f};
    for (const auto &p : c.port_pressure)
        if (p.second > best.second)
            best = p;
    return best;
}

/// Escapa un texto para un label mermaid entre comillas: comillas -> &quot;,
/// saltos de linea -> <br/>, y neutraliza los caracteres que rompen el parser.
std::string esc_mermaid(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': o += "&quot;"; break;
        case '\n': o += "<br/>"; break;
        case '[': o += "&#91;"; break;
        case ']': o += "&#93;"; break;
        case '{': o += "&#123;"; break;
        case '}': o += "&#125;"; break;
        case '|': o += "&#124;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        default: o += c; break;
        }
    }
    return o;
}

/// Escapa para un label graphviz entre comillas.
std::string esc_dot(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        if (c == '\n') {
            o += "\\l"; // alineado a la izquierda en graphviz.
            continue;
        }
        o += c;
    }
    return o;
}

/// Coste (latencia serie + throughput superescalar + cuello) de un rango de
/// instrucciones (las de un bloque basico), reuniendo su texto.
struct BlockCost {
    float latency = 0.0f;
    float throughput = 0.0f;
    std::string port; // cuello de botella
    float port_uops = 0.0f;
    bool writes_flags = false;
    bool reads_flags = false;
};

BlockCost block_cost(const AsmCfg &cfg, const AsmBasicBlock &bb,
                     const AsmDiagramOptions &opt) {
    std::string body;
    for (uint32_t i = bb.first; i <= bb.last; ++i) {
        body += cfg.insns[i].text;
        body += '\n';
    }
    instr_db::AsmBlockCost c =
        instr_db::analyze_asm_cost(opt.isa, body, opt.ua_id);
    BlockCost r;
    r.latency = c.latency_sum;
    r.throughput = c.throughput;
    auto bn = bottleneck(c);
    r.port = bn.first;
    r.port_uops = bn.second;
    for (uint32_t i = bb.first; i <= bb.last; ++i) {
        instr_db::AsmInsnSem s =
            instr_db::asm_insn_sem(opt.isa, cfg.insns[i].text, opt.ua_id);
        r.writes_flags = r.writes_flags || s.writes_flags;
        r.reads_flags = r.reads_flags ||
                        (cfg.insns[i].term == AsmTerm::CondBranch) ||
                        s.reads_flags;
    }
    return r;
}

/// Marcadores compactos de flags para el label de un bloque.
std::string flag_marks(const BlockCost &bc) {
    std::string m;
    if (bc.writes_flags)
        m += " Fw"; // escribe flags
    if (bc.reads_flags)
        m += " Fr"; // lee flags
    return m;
}

/// Texto del cuerpo de un bloque (instrucciones, con su etiqueta si tiene),
/// truncado a 6 instrucciones para legibilidad.
std::string block_body_text(const AsmCfg &cfg, const AsmBasicBlock &bb) {
    std::string t;
    if (!bb.label.empty())
        t += bb.label + ":\n";
    const uint32_t n = bb.last - bb.first + 1;
    const uint32_t show = n > 6 ? 5 : n;
    for (uint32_t k = 0; k < show; ++k)
        t += cfg.insns[bb.first + k].text + "\n";
    if (n > 6)
        t += "... (" + std::to_string(n) + " instrs)\n";
    return t;
}

} // namespace

std::string asm_cfg_mermaid(const std::string &body,
                            const AsmDiagramOptions &opt) {
    AsmCfg cfg = build_asm_cfg(opt.isa, body);
    instr_db::AsmBlockCost total =
        instr_db::analyze_asm_cost(opt.isa, body, opt.ua_id);
    auto bn = bottleneck(total);
    std::vector<AsmDiag> diags = asm_diagnose_cfg(cfg);

    std::ostringstream os;
    const std::string &p = opt.id_prefix;

    // Titulo del subgrafo: microarq + coste total + cuello de botella.
    std::string head = opt.title.empty() ? std::string("asm") : opt.title;
    if (!opt.microarch.empty())
        head += " @ " + opt.microarch;
    head += "\nlat " + f1(total.latency_sum) + "c (serie) / thr " +
            f1(total.throughput) + "c (superescalar)";
    if (!bn.first.empty())
        head += "\ncuello: " + bn.first + " (" + f1(bn.second) + " uops)";
    head += "\n" + std::to_string(total.matched) + "/" +
            std::to_string(total.instr_count) + " instrs modeladas";

    os << "  subgraph " << p << " [\"" << esc_mermaid(head) << "\"]\n";
    os << "    direction TB\n";

    // Un nodo por bloque basico con su cuerpo + coste.
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        BlockCost bc = block_cost(cfg, cfg.blocks[b], opt);
        std::string lbl = block_body_text(cfg, cfg.blocks[b]);
        lbl += "[lat " + f1(bc.latency) + "c / thr " + f1(bc.throughput) + "c";
        if (!bc.port.empty())
            lbl += " / " + bc.port;
        lbl += flag_marks(bc);
        lbl += "]";
        os << "    " << p << "_b" << b << " [\"" << esc_mermaid(lbl) << "\"]\n";
    }

    // Aristas.
    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        for (uint32_t s : cfg.blocks[b].succs) {
            const char *style = (s <= b) ? " -.->|back| " : " --> ";
            os << "    " << p << "_b" << b << style << p << "_b" << s << "\n";
        }
    }

    // Nodo de diagnosticos (si hay).
    if (!diags.empty()) {
        std::string dl = "diagnosticos:\n";
        for (const AsmDiag &d : diags) {
            dl += d.code;
            for (const auto &a : d.args) dl += " " + a;
            dl += "\n";
        }
        os << "    " << p << "_diag [\"" << esc_mermaid(dl) << "\"]\n";
        os << "    style " << p << "_diag fill:#fee,stroke:#c33\n";
    }

    os << "  end\n";
    return os.str();
}

std::string asm_cfg_graphviz(const std::string &body,
                             const AsmDiagramOptions &opt) {
    AsmCfg cfg = build_asm_cfg(opt.isa, body);
    instr_db::AsmBlockCost total =
        instr_db::analyze_asm_cost(opt.isa, body, opt.ua_id);
    auto bn = bottleneck(total);
    std::vector<AsmDiag> diags = asm_diagnose_cfg(cfg);

    std::ostringstream os;
    const std::string &p = opt.id_prefix;

    std::string head = opt.title.empty() ? std::string("asm") : opt.title;
    if (!opt.microarch.empty())
        head += " @ " + opt.microarch;
    head += "\\nlat " + f1(total.latency_sum) + "c / thr " +
            f1(total.throughput) + "c";
    if (!bn.first.empty())
        head += "\\ncuello: " + bn.first + " (" + f1(bn.second) + " uops)";

    os << "  subgraph cluster_" << p << " {\n";
    os << "    label=\"" << esc_dot(head) << "\";\n";
    os << "    style=rounded; color=\"#3366aa\";\n";
    os << "    node [shape=box, fontname=\"monospace\"];\n";

    for (uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        BlockCost bc = block_cost(cfg, cfg.blocks[b], opt);
        std::string lbl = block_body_text(cfg, cfg.blocks[b]);
        lbl += "[lat " + f1(bc.latency) + "c / thr " + f1(bc.throughput) + "c";
        if (!bc.port.empty())
            lbl += " / " + bc.port;
        lbl += flag_marks(bc);
        lbl += "]";
        os << "    " << p << "_b" << b << " [label=\"" << esc_dot(lbl)
           << "\"];\n";
    }

    for (uint32_t b = 0; b < cfg.blocks.size(); ++b)
        for (uint32_t s : cfg.blocks[b].succs) {
            const bool back = (s <= b);
            os << "    " << p << "_b" << b << " -> " << p << "_b" << s;
            if (back)
                os << " [style=dashed, label=\"back\"]";
            os << ";\n";
        }

    if (!diags.empty()) {
        std::string dl = "diagnosticos:\\n";
        for (const AsmDiag &d : diags) {
            dl += d.code;
            for (const auto &a : d.args) dl += " " + a;
            dl += "\\n";
        }
        os << "    " << p << "_diag [label=\"" << esc_dot(dl)
           << "\", style=filled, fillcolor=\"#ffeeee\", color=\"#cc3333\"];\n";
    }

    os << "  }\n";
    return os.str();
}

} // namespace vex
