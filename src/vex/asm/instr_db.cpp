/**
 * @file instr_db.cpp
 * @brief Emparejador texto->FormID sobre la DB de instrucciones EMBEBIDA.
 *
 * Las tablas por-ISA las genera @c tools/import/gen_cpp_db.py (ficheros
 * @c gen/instr_db_<isa>_gen.inc, incluidos aqui como @c .rodata estatica).  El
 * @c match espeja el emparejador del analizador: rango del iclass por busqueda
 * binaria + puntuacion por clase/ancho de operando.
 */
#include "vex/asm/instr_db.h"

#include "vex/asm/asm_effects.h" // asm_canonical_reg (colapsa alias de registro)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace vex {
namespace instr_db {

namespace {

/// Tablas de una ISA (las rellena el accesor @c db_<isa>() del .cpp generado).
IsaData tables_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return db_x86();
    case Isa::ARM64:
        return db_arm64();
    case Isa::ARM32:
        return db_arm32();
    case Isa::RISCV:
        return db_riscv();
    }
    return {};
}

/// Operandos EXPLICITOS de una forma para puntuar (descarta implicit/suppressed
/// y los operandos de flags, que el usuario no escribe).
void explicit_ops(const IsaData &t, const DbForm &f,
                  std::vector<const DbOperand *> &out) {
    out.clear();
    for (unsigned i = 0; i < f.ops_count; ++i) {
        const DbOperand &o = t.ops[f.ops_off + i];
        if (o.flags & 0x0C) continue;          // implicit(bit2) | suppressed(bit3)
        if (o.kind == OP_FLAGS) continue;
        out.push_back(&o);
    }
}

/// Puntua los operandos del usuario contra los de la forma; -1 si no casan.
int score_ops(const std::vector<ParsedOp> &user,
              const std::vector<const DbOperand *> &form) {
    if (user.size() != form.size()) return -1;
    int s = 0;
    for (size_t i = 0; i < form.size(); ++i) {
        const ParsedOp &u = user[i];
        const DbOperand &fo = *form[i];
        if (u.kind != fo.kind) return -1;
        if (u.width && fo.width) {
            if (u.width != fo.width) return -1;
            s += 2;
        } else {
            s += 1;
        }
    }
    return s;
}

/// Busca el rango de FormIDs de un mnemonico (binaria sobre kIclassIndex,
/// ordenado por nombre).  Devuelve nullptr si no existe.
const DbIclassRange *find_iclass(const IsaData &t, const std::string &up) {
    unsigned lo = 0, hi = t.iclass_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const DbIclassRange &r = t.iclass[mid];
        int c = std::strcmp(t.str[r.iclass], up.c_str());
        if (c == 0) return &r;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

} // namespace

namespace {

/// Ancho (bits) de un registro segun la ISA (0 = desconocido/no restringe).
/// Espeja @c regWidth del analizador (x86 exacto; ARM/RISC-V el ancho real,
/// pero las formas ARM/RISC-V llevan width 0 = "cualquiera", asi que el score
/// no lo restringe).
uint16_t reg_width(Isa isa, const std::string &r) {
    auto pref = [&](const char *p) {
        size_t n = std::strlen(p);
        return r.size() > n && r.compare(0, n, p) == 0 &&
               std::isdigit((unsigned char)r[n]);
    };
    if (isa == Isa::ARM64 || isa == Isa::ARM32) {
        if (r == "sp" || r == "xzr" || r == "lr") return 64;
        if (r == "wzr" || r == "wsp" || r == "pc") return 32;
        if (!r.empty() && (r[0] == 'x') && std::isdigit((unsigned char)r[1]))
            return 64;
        if (!r.empty() && (r[0] == 'w') && std::isdigit((unsigned char)r[1]))
            return 32;
        if (!r.empty() && (r[0] == 'r') && std::isdigit((unsigned char)r[1]))
            return 32; // A32
        if (pref("q") || pref("v")) return 128;
        if (pref("d")) return 64;
        if (pref("s")) return 32;
        if (pref("h")) return 16;
        if (pref("b")) return 8;
        return 0;
    }
    if (isa == Isa::RISCV) {
        static const char *abi[] = {
            "zero", "ra", "sp", "gp", "tp", "fp", "t0", "t1", "t2", "t3", "t4",
            "t5", "t6", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8",
            "s9", "s10", "s11", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
        for (const char *a : abi)
            if (r == a) return 64;
        if ((r[0] == 'x' || r[0] == 'f') && r.size() > 1 &&
            std::isdigit((unsigned char)r[1]))
            return 64;
        if (r.size() >= 2 && (r.compare(0, 2, "ft") == 0 ||
                              r.compare(0, 2, "fs") == 0 ||
                              r.compare(0, 2, "fa") == 0))
            return 64;
        return 0;
    }
    // x86: anchos exactos.
    auto is = [&](std::initializer_list<const char *> l) {
        for (auto s : l)
            if (r == s) return true;
        return false;
    };
    if (is({"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp"}) ||
        (pref("r") && (r.back() != 'd' && r.back() != 'w' && r.back() != 'b')))
        return 64;
    if (is({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}) ||
        (pref("r") && r.back() == 'd'))
        return 32;
    if (is({"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}) ||
        (pref("r") && r.back() == 'w'))
        return 16;
    if (is({"al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "sil", "dil",
            "bpl", "spl"}) ||
        (pref("r") && r.back() == 'b'))
        return 8;
    if (pref("xmm")) return 128;
    if (pref("ymm")) return 256;
    if (pref("zmm")) return 512;
    if (pref("k")) return 64;
    return 0;
}

} // namespace

ParsedOp parse_operand(Isa isa, const std::string &token) {
    std::string t = token;
    // trim.
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(0, 1);
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    std::string low = t;
    for (char &c : low) c = static_cast<char>(std::tolower((unsigned char)c));

    // RISC-V: memoria como desplazamiento(reg): 0(a0), -4(sp).
    if (isa == Isa::RISCV) {
        size_t op = low.find('(');
        if (op != std::string::npos && low.back() == ')')
            return ParsedOp{OP_MEM, 0};
    }
    // x86/ARM: memoria [...].
    if (low.find('[') != std::string::npos) return ParsedOp{OP_MEM, 0};
    // inmediato ARM (#imm) o numero.
    std::string num = low;
    if (!num.empty() && num[0] == '#') num.erase(0, 1);
    if (!num.empty() &&
        (std::isdigit((unsigned char)num[0]) ||
         ((num[0] == '-' || num[0] == '+') && num.size() > 1)))
        return ParsedOp{OP_IMM, 0};
    // registro.
    uint16_t w = reg_width(isa, low);
    if (w || (!low.empty() && std::isalpha((unsigned char)low[0])))
        return ParsedOp{OP_REG, w};
    return ParsedOp{OP_IMM, 0};
}

namespace {

/// trim in-place.
void trim(std::string &s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
}

/// Parte una linea de asm en mnemonico + tokens de operando (respetando
/// @c [...] y @c (...)).  Devuelve false si es vacia o label.
bool split_asm_line(const std::string &line, std::string &mnem,
                    std::vector<std::string> &toks) {
    std::string s = line;
    size_t cm = s.find(';');
    if (cm != std::string::npos) s.resize(cm);
    size_t sl = s.find("//");
    if (sl != std::string::npos) s.resize(sl);
    trim(s);
    if (s.empty() || s.back() == ':') return false;
    size_t sp = s.find_first_of(" \t");
    mnem = sp == std::string::npos ? s : s.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : s.substr(sp + 1);
    toks.clear();
    std::string cur;
    int depth = 0;
    auto flush = [&]() {
        trim(cur);
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
    };
    for (char c : rest) {
        if (c == '[' || c == '(')
            ++depth;
        else if (c == ']' || c == ')')
            --depth;
        if (c == ',' && depth == 0)
            flush();
        else
            cur += c;
    }
    flush();
    return true;
}

/// Registro canonico (colapsa alias de ancho) para el grafo de dependencias.
std::string canon_reg(Isa isa, const std::string &tok) {
    std::string low = tok;
    for (char &c : low) c = static_cast<char>(std::tolower((unsigned char)c));
    if (isa == Isa::RISCV) {
        // nombres ABI -> registro fisico (a0=x10, ra=x1, ...).
        static const std::pair<const char *, const char *> abi[] = {
            {"zero", "x0"}, {"ra", "x1"}, {"sp", "x2"}, {"gp", "x3"},
            {"tp", "x4"},   {"fp", "x8"}, {"t0", "x5"}, {"t1", "x6"},
            {"t2", "x7"},   {"s0", "x8"}, {"s1", "x9"}, {"a0", "x10"},
            {"a1", "x11"},  {"a2", "x12"}, {"a3", "x13"}, {"a4", "x14"},
            {"a5", "x15"},  {"a6", "x16"}, {"a7", "x17"}, {"s2", "x18"},
            {"s3", "x19"},  {"s4", "x20"}, {"s5", "x21"}, {"s6", "x22"},
            {"s7", "x23"},  {"s8", "x24"}, {"s9", "x25"}, {"s10", "x26"},
            {"s11", "x27"}, {"t3", "x28"}, {"t4", "x29"}, {"t5", "x30"},
            {"t6", "x31"}};
        for (const auto &p : abi)
            if (low == p.first) return p.second;
        return low;
    }
    // x86 y ARM64 (colapsa rax/eax, x0/w0...).  ARM32 sin alias de ancho.
    return vex::asm_canonical_reg(low);
}

/// Registros que aparecen dentro de un operando de memoria (direccion).
void addr_regs(Isa isa, const std::string &tok, std::vector<std::string> &out) {
    // extrae subtokens alfanumericos y quedate con los que son registros.
    std::string cur;
    auto push = [&]() {
        if (!cur.empty()) {
            uint16_t w = 0;
            (void)w;
            // reusa parse_operand para saber si es registro.
            ParsedOp p = parse_operand(isa, cur);
            if (p.kind == OP_REG) out.push_back(canon_reg(isa, cur));
            cur.clear();
        }
    };
    for (char c : tok) {
        if (std::isalnum((unsigned char)c) || c == '_' || c == '.')
            cur += c;
        else
            push();
    }
    push();
}

} // namespace

int32_t match_asm_line(Isa isa, const std::string &line) {
    std::string mnem;
    std::vector<std::string> toks;
    if (!split_asm_line(line, mnem, toks)) return -1;
    std::vector<ParsedOp> ops;
    ops.reserve(toks.size());
    for (const auto &t : toks) ops.push_back(parse_operand(isa, t));
    return match(isa, mnem, ops);
}

int32_t match(Isa isa, const std::string &mnemonic,
              const std::vector<ParsedOp> &ops) {
    const IsaData t = tables_for(isa);
    if (!t.forms) return -1;
    std::string up = mnemonic;
    for (char &c : up) c = static_cast<char>(std::toupper((unsigned char)c));
    const DbIclassRange *r = find_iclass(t, up);
    if (!r) return -1;                          // mnemonico no existe
    int32_t best = -1;
    int best_s = -1;
    std::vector<const DbOperand *> fo;
    for (uint32_t fid = r->first_fid; fid < r->first_fid + r->count; ++fid) {
        explicit_ops(t, t.forms[fid], fo);
        int s = score_ops(ops, fo);
        if (s > best_s) {
            best_s = s;
            best = static_cast<int32_t>(fid);
        }
    }
    // El mnemonico existe: si nada caso por operandos, vale la primera del rango.
    return best >= 0 ? best : static_cast<int32_t>(r->first_fid);
}

const char *iclass_name(Isa isa, int32_t form_id) {
    const IsaData t = tables_for(isa);
    if (!t.forms || form_id < 0 ||
        static_cast<unsigned>(form_id) >= t.form_count)
        return "";
    return t.str[t.forms[form_id].iclass];
}

uint16_t overlay_of(Isa isa, int32_t form_id) {
    const IsaData t = tables_for(isa);
    if (!t.forms || form_id < 0 ||
        static_cast<unsigned>(form_id) >= t.form_count)
        return 0;
    return t.forms[form_id].overlay;
}

uint32_t form_count(Isa isa) { return tables_for(isa).form_count; }

// -------------------------------------------------------------------------
// Capa de coste: latencia + puertos por microarquitectura.
// -------------------------------------------------------------------------

namespace {
CostData cost_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return cost_x86();
    case Isa::ARM64:
        return cost_arm64();
    case Isa::ARM32:
        return cost_arm32();
    case Isa::RISCV:
        return cost_riscv();
    }
    return {};
}
} // namespace

uint32_t microarch_count(Isa isa) { return cost_for(isa).count; }

const char *microarch_name(Isa isa, uint32_t ua_id) {
    const CostData c = cost_for(isa);
    return ua_id < c.count ? c.uarchs[ua_id].name : "";
}

int32_t microarch_by_name(Isa isa, const std::string &name) {
    const CostData c = cost_for(isa);
    for (uint32_t i = 0; i < c.count; ++i)
        if (name == c.uarchs[i].name) return static_cast<int32_t>(i);
    return -1;
}

AsmCost cost(Isa isa, int32_t form_id, uint32_t ua_id) {
    AsmCost r;
    const CostData c = cost_for(isa);
    if (ua_id >= c.count || form_id < 0) return r;
    const MicroarchData &m = c.uarchs[ua_id];
    if (static_cast<uint32_t>(form_id) >= m.form_count) return r;
    int16_t cid = m.form_class[form_id];
    if (cid < 0) return r;                       // no cronometrada en esta uarch
    const AsmClass &cl = m.classes[cid];
    r.found = true;
    r.recip_tp = cl.recip_tp;
    r.latency = cl.latency;
    r.div_cycles = cl.div_cycles;
    r.uops = cl.uops;
    r.microcoded = (cl.flags & 0x1) != 0;
    r.macro_fusible = (cl.flags & 0x2) != 0;
    r.ports = m.slots + cl.ports_off;
    r.ports_count = cl.ports_count;
    r.port_names = m.port_names;
    return r;
}

// -------------------------------------------------------------------------
// Capa de features por CPU (que extensiones de ISA admite cada core).
// -------------------------------------------------------------------------

namespace {
FeatData feat_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return feat_x86();
    case Isa::ARM64:
    case Isa::ARM32:
        return feat_arm();          // A64 y A32/T32 comparten features.
    case Isa::RISCV:
        return feat_riscv();
    }
    return {};
}
} // namespace

uint32_t cpu_count(Isa isa) { return feat_for(isa).cpu_count; }

const char *cpu_name(Isa isa, uint32_t cpu_id) {
    const FeatData f = feat_for(isa);
    return cpu_id < f.cpu_count ? f.cpus[cpu_id].name : "";
}

int32_t cpu_by_name(Isa isa, const std::string &name) {
    const FeatData f = feat_for(isa);
    for (uint32_t i = 0; i < f.cpu_count; ++i)
        if (name == f.cpus[i].name) return static_cast<int32_t>(i);
    return -1;
}

bool cpu_has_feature(Isa isa, uint32_t cpu_id, const std::string &feature) {
    const FeatData f = feat_for(isa);
    if (cpu_id >= f.cpu_count) return false;
    const CpuFeatures &c = f.cpus[cpu_id];
    for (uint16_t i = 0; i < c.feat_count; ++i)
        if (feature == f.feat_names[c.feats[i]]) return true;
    return false;
}

AsmBlockCost analyze_asm_cost(Isa isa, const std::string &body,
                              uint32_t ua_id) {
    AsmBlockCost out;
    // presion por puerto acumulada por NOMBRE (dos instrucciones distintas
    // pueden compartir grupo de puertos).
    std::vector<std::pair<std::string, float>> pressure;
    auto add_port = [&](const std::string &name, float uops) {
        for (auto &p : pressure)
            if (p.first == name) {
                p.second += uops;
                return;
            }
        pressure.emplace_back(name, uops);
    };
    float tp_sum = 0.0f;

    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string line =
            body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        int32_t fid = match_asm_line(isa, line);
        if (fid < 0) {
            // ¿linea con contenido pero mnemonico desconocido?  cuenta como
            // instruccion no emparejada (para la completitud).
            std::string t = line;
            size_t c = t.find_first_of(";");
            if (c != std::string::npos) t.resize(c);
            size_t s2 = t.find("//");
            if (s2 != std::string::npos) t.resize(s2);
            while (!t.empty() && std::isspace((unsigned char)t.front()))
                t.erase(0, 1);
            while (!t.empty() && std::isspace((unsigned char)t.back()))
                t.pop_back();
            if (!t.empty() && t.back() != ':') ++out.instr_count;
            continue;
        }
        ++out.instr_count;
        ++out.matched;
        AsmCost c = cost(isa, fid, ua_id);
        if (!c.found) continue;
        ++out.costed;
        out.total_uops += c.uops;
        out.latency_sum += c.latency;
        tp_sum += c.recip_tp;
        for (uint8_t k = 0; k < c.ports_count; ++k) {
            const AsmPortSlot &ps = c.ports[k];
            add_port(c.port_names[ps.port], ps.uops);
        }
    }
    // throughput = max(puerto mas cargado, suma de recip_tp) -- cota inferior de
    // ciclos del bloque bien planificado (ejecucion paralela por puertos).
    float max_port = 0.0f;
    for (const auto &p : pressure) max_port = std::max(max_port, p.second);
    out.throughput = std::max(max_port, tp_sum);
    out.port_pressure = std::move(pressure);
    return out;
}

// -------------------------------------------------------------------------
// Scheduling: semantica por instruccion + hazards + list scheduling.
// -------------------------------------------------------------------------

namespace {
bool contains(const std::vector<std::string> &v, const std::string &x) {
    for (const auto &e : v)
        if (e == x) return true;
    return false;
}

/// Todo bit de overlay que impide reordenar (barrera dura).
const uint16_t OVL_BARRIER_ANY =
    OVL_BARRIER | OVL_SERIALIZING | OVL_ATOMIC | OVL_LL_SC | OVL_MEM_ACQUIRE |
    OVL_MEM_RELEASE | OVL_MEM_SEQ_CST | OVL_NO_REORDER | OVL_BRANCH | OVL_CALL |
    OVL_RET | OVL_SYSCALL;
} // namespace

AsmInsnSem asm_insn_sem(Isa isa, const std::string &line, uint32_t ua_id) {
    AsmInsnSem s;
    s.text = line;
    std::string mnem;
    std::vector<std::string> toks;
    if (!split_asm_line(line, mnem, toks)) {
        s.form_id = -1;                          // label / vacia: no es instruccion
        return s;
    }
    std::vector<ParsedOp> ops;
    ops.reserve(toks.size());
    for (const auto &t : toks) ops.push_back(parse_operand(isa, t));
    int32_t fid = match(isa, mnem, ops);
    s.form_id = fid;
    if (fid < 0) {                               // mnemonico desconocido
        s.barrier = true;                        // CONSERVADOR: no se reordena.
        s.modeled = false;
        return s;
    }
    const IsaData tb = tables_for(isa);
    const DbForm &f = tb.forms[fid];
    s.barrier = (f.overlay & OVL_BARRIER_ANY) != 0;
    s.writes_flags = (f.memflags & 0x04) != 0;
    s.reads_flags = (f.memflags & 0x08) != 0;
    s.latency = cost(isa, fid, ua_id).latency;

    // Operandos EXPLICITOS del form (no implicit/suppressed, no flags) alineados
    // con los tokens de la linea.  Si hay registros IMPLICITOS, la aridad no casa
    // o toca memoria no capturada -> CONSERVADOR (no modelada).
    std::vector<int> expl;
    bool implicit_reg = false;
    bool mem_operand = false;
    for (unsigned i = 0; i < f.ops_count; ++i) {
        const DbOperand &o = tb.ops[f.ops_off + i];
        if (o.kind == OP_FLAGS) continue;
        bool impl = (o.flags & 0x0C) != 0;
        if (impl) {
            if (o.kind == OP_REG) implicit_reg = true;
            continue;
        }
        if (o.kind == OP_MEM) mem_operand = true;
        expl.push_back(static_cast<int>(i));
    }
    bool arity_ok = expl.size() == toks.size();
    // memoria implicita (memflags bit0 sin operando mem, p.ej. push/pop) -> no
    // modelada.
    bool implicit_mem = (f.memflags & 0x01) != 0 && !mem_operand;
    s.modeled = arity_ok && !implicit_reg && !implicit_mem;

    if (arity_ok) {
        for (size_t k = 0; k < expl.size(); ++k) {
            int i = expl[k];
            const DbOperand &o = tb.ops[f.ops_off + i];
            bool rd = (f.rmask >> i) & 1;
            bool wr = (f.wmask >> i) & 1;
            if (o.kind == OP_REG) {
                std::string cr = canon_reg(isa, toks[k]);
                if (rd) s.reads.push_back(cr);
                if (wr) s.writes.push_back(cr);
            } else if (o.kind == OP_MEM) {
                if (rd) s.reads_mem = true;
                if (wr) s.writes_mem = true;
                addr_regs(isa, toks[k], s.reads); // los regs de direccion se leen
            }
        }
    }
    if (implicit_mem) {                          // conservador: asume R/W memoria
        s.reads_mem = true;
        s.writes_mem = true;
    }
    return s;
}

bool asm_dep_conflict(const AsmInsnSem &a, const AsmInsnSem &b) {
    // barrera o no-modelada -> siempre conflicto (no se reordena alrededor).
    if (a.barrier || b.barrier || !a.modeled || !b.modeled) return true;
    // memoria: si alguna ESCRIBE y la otra toca memoria (no se sabe si solapan).
    bool amem = a.reads_mem || a.writes_mem;
    bool bmem = b.reads_mem || b.writes_mem;
    if ((a.writes_mem && bmem) || (b.writes_mem && amem)) return true;
    // flags: WAW / WAR / RAW.
    if ((a.writes_flags && (b.reads_flags || b.writes_flags)) ||
        (b.writes_flags && a.reads_flags))
        return true;
    // registros: RAW (a escribe -> b lee), WAW (ambos escriben),
    // WAR (a lee -> b escribe).
    for (const auto &w : a.writes)
        if (contains(b.reads, w) || contains(b.writes, w)) return true;
    for (const auto &w : b.writes)
        if (contains(a.reads, w)) return true;
    return false;
}

AsmSchedule schedule_asm_block(Isa isa, const std::string &body,
                               uint32_t ua_id) {
    AsmSchedule out;
    // parte en instrucciones (ignora labels/vacias).
    std::vector<AsmInsnSem> sem;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(
            i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        std::string mnem;
        std::vector<std::string> toks;
        if (!split_asm_line(line, mnem, toks)) continue; // label / vacia
        sem.push_back(asm_insn_sem(isa, line, ua_id));
    }
    const uint32_t n = static_cast<uint32_t>(sem.size());
    for (uint32_t k = 0; k < n; ++k) out.order.push_back(k);
    if (n < 2) return out;

    // planifica por SEGMENTOS: una barrera corta el bloque (nada la cruza).
    std::vector<uint32_t> result;
    uint32_t seg_start = 0;
    auto sched_segment = [&](uint32_t lo, uint32_t hi) {
        const uint32_t m = hi - lo;
        if (m <= 1) {
            for (uint32_t k = lo; k < hi; ++k) result.push_back(k);
            return;
        }
        // aristas de dependencia i->j (i antes que j en el original y conflictan).
        std::vector<std::vector<uint32_t>> succ(m);
        std::vector<uint32_t> indeg(m, 0);
        for (uint32_t x = 0; x < m; ++x)
            for (uint32_t y = x + 1; y < m; ++y)
                if (asm_dep_conflict(sem[lo + x], sem[lo + y])) {
                    succ[x].push_back(y);
                    ++indeg[y];
                }
        // altura = latencia + max altura de sucesores (camino critico).
        std::vector<float> height(m, 0.0f);
        for (int x = static_cast<int>(m) - 1; x >= 0; --x) {
            float h = 0.0f;
            for (uint32_t sIdx : succ[x]) h = std::max(h, height[sIdx]);
            height[x] = sem[lo + x].latency + h;
        }
        // list scheduling: entre los listos, el de mayor altura (desempate:
        // orden original -> estable).
        std::vector<uint8_t> done(m, 0);
        std::vector<uint32_t> rem = indeg;
        for (uint32_t step = 0; step < m; ++step) {
            int pick = -1;
            for (uint32_t x = 0; x < m; ++x) {
                if (done[x] || rem[x] != 0) continue;
                if (pick < 0 || height[x] > height[pick] + 1e-6f)
                    pick = static_cast<int>(x);
            }
            done[pick] = 1;
            result.push_back(lo + static_cast<uint32_t>(pick));
            for (uint32_t sIdx : succ[pick]) --rem[sIdx];
        }
    };
    for (uint32_t k = 0; k < n; ++k) {
        if (sem[k].barrier) {
            sched_segment(seg_start, k);
            result.push_back(k);                 // la barrera se queda en su sitio
            seg_start = k + 1;
        }
    }
    sched_segment(seg_start, n);

    out.order = result;
    out.moved = false;
    for (uint32_t k = 0; k < n; ++k)
        if (out.order[k] != k) {
            out.moved = true;
            break;
        }
    // INVARIANTE de seguridad: ningun par en conflicto queda invertido.
    std::vector<uint32_t> pos(n);
    for (uint32_t k = 0; k < n; ++k) pos[out.order[k]] = k;
    for (uint32_t x = 0; x < n && out.valid; ++x)
        for (uint32_t y = x + 1; y < n; ++y)
            if (asm_dep_conflict(sem[x], sem[y]) && pos[x] > pos[y]) {
                out.valid = false;
                break;
            }
    return out;
}

namespace {
/// ¿La linea es (o empieza por) una etiqueta `nombre:`?
bool line_has_label(const std::string &line) {
    std::string s = line;
    size_t cm = s.find(';');
    if (cm != std::string::npos) s.resize(cm);
    size_t sl = s.find("//");
    if (sl != std::string::npos) s.resize(sl);
    trim(s);
    if (s.empty()) return false;
    size_t k = 0;
    if (!(std::isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == '.'))
        return false;
    while (k < s.size() &&
           (std::isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '.'))
        ++k;
    while (k < s.size() && std::isspace((unsigned char)s[k])) ++k;
    return k < s.size() && s[k] == ':';
}
} // namespace

std::string reschedule_asm(Isa isa, const std::string &body, uint32_t ua_id) {
    // Recolecta las lineas de INSTRUCCION (texto original) en el mismo orden que
    // el scheduler; si aparece una etiqueta -> NO se reordena (conservador).
    std::vector<std::string> insns;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(
            i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        if (line_has_label(line)) return body; // label -> no tocar
        std::string mnem;
        std::vector<std::string> toks;
        if (split_asm_line(line, mnem, toks)) insns.push_back(line);
    }
    if (insns.size() < 2) return body;

    AsmSchedule sc = schedule_asm_block(isa, body, ua_id);
    if (!sc.valid || !sc.moved || sc.order.size() != insns.size())
        return body; // no seguro / no mejora -> original

    std::string out;
    for (size_t k = 0; k < sc.order.size(); ++k) {
        // recorta espacios de cabecera para reindentar uniforme.
        std::string t = insns[sc.order[k]];
        trim(t);
        out += t;
        out += '\n';
    }
    return out;
}

} // namespace instr_db
} // namespace vex
