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
 * @file asm_analyze.cpp
 * @brief Implementacion del resumen de efectos de bloque de un inline asm
 *        (ver asm_analyze.h).
 */
#include "vex/asm/asm_analyze.h"

#include <cctype>
#include <cstdlib>

#include "vex/asm/asm_effects.h"

namespace vex {

namespace {

/// Pasa @p s a minusculas (ASCII).
std::string lower(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s)
        o.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    return o;
}

/// Trocea una linea NASM en tokens: separadores whitespace y coma; los
/// corchetes/operandos se mantienen como tokens sueltos (basta para detectar
/// el mnemonico y los registros; el analisis fino de operandos es trabajo
/// posterior (reconstruccion de CFG + dataflow).
std::vector<std::string> tokenize_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    for (char c : line) {
        if (std::isspace((unsigned char)c) || c == ',') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

/// @c true si @p mnem es una rama/salto (para @c has_branch).  Cubre x86
/// (jmp/jCC/loop) y arm64 (b/b.CC/cbz/cbnz/tbz/tbnz).
bool es_rama(const std::string &mnem) {
    if (mnem.empty()) return false;
    if (mnem == "jmp" || mnem == "loop" || mnem == "loope" ||
        mnem == "loopne")
        return true;
    if (mnem[0] == 'j' && mnem.size() >= 2) return true; // jCC (je/jne/jg/...)
    // arm64: b, b.CC, bl, br, cbz, cbnz, tbz, tbnz.
    if (mnem == "b" || mnem == "bl" || mnem == "br" || mnem == "cbz" ||
        mnem == "cbnz" || mnem == "tbz" || mnem == "tbnz")
        return true;
    if (mnem.size() > 2 && mnem[0] == 'b' && mnem[1] == '.')
        return true; // b.ne / b.eq / ...
    return false;
}

/// @c true si @p mnem es una atomica de arm64 (LL/SC o CAS/RMW/swap): base para
/// @c has_atomic.  Se reconoce por prefijo para cubrir todas las variantes de
/// orden/ancho (ldaxr/ldaxrb/..., cas/casa/casal/..., ldadd*/ldset*/swp*).
bool es_atomica_arm(const std::string &mnem) {
    static const char *pref[] = {"ldaxr", "ldxr",  "ldar", "stlxr", "stxr",
                                 "stlr",  "ldaxp", "ldxp", "stlxp", "stxp",
                                 "cas",   "swp",   "ldadd", "ldset", "ldclr",
                                 "ldeor"};
    for (const char *p : pref)
        if (mnem.rfind(p, 0) == 0) return true;
    return false;
}

/// Ancho (bytes) del slot de pila de un @c push/@c pop segun el arch x86.
int stack_word_x86(const std::string &arch) {
    if (arch == "x86_16") return 2;
    if (arch == "x86") return 4; // x86-32
    return 8;                     // x86_64
}

/// Delta de pila EXPLICITO de una instruccion, en bytes (positivo = mas marco).
/// x86: @c push/@c pop (ancho por arch) y @c sub/@c add sobre @c rsp/@c esp/
/// @c sp; arm64: @c sub/@c add sobre @c sp.  Pone @p ok=false si mueve el
/// puntero de pila con un inmediato NO literal (conservador: no acotable).
int64_t stack_delta(const std::vector<std::string> &toks, size_t mi,
                    const std::string &arch, bool &ok) {
    ok = true;
    const std::string m = lower(toks[mi]);
    const bool x86 = (arch.rfind("x86", 0) == 0 || arch.empty());
    if (x86) {
        const int w = stack_word_x86(arch);
        if (m == "push") return w;
        if (m == "pop") return -w;
    }
    // sub/add <sp>, <imm> (x86: rsp/esp/sp; arm64: `sub sp, sp, #imm`).
    if ((m == "sub" || m == "add") && mi + 2 < toks.size()) {
        const std::string dst = lower(toks[mi + 1]);
        const bool is_sp =
            (dst == "rsp" || dst == "esp" || dst == "sp");
        if (is_sp) {
            // arm64: `sub sp, sp, #imm` -> el inmediato es el 3er operando y
            // lleva '#'.  x86: `sub rsp, imm` -> 2o operando.
            std::string imm = (!x86 && mi + 3 < toks.size())
                                   ? toks[mi + 3]
                                   : toks[mi + 2];
            if (!imm.empty() && imm[0] == '#') imm = imm.substr(1);
            char *end = nullptr;
            const long v = std::strtol(imm.c_str(), &end, 0);
            if (end != imm.c_str() && *end == '\0') {
                const int64_t d = static_cast<int64_t>(v);
                return (m == "sub") ? d : -d;
            }
            ok = false; // sp movido con algo no literal.
            return 0;
        }
    }
    return 0;
}

} // namespace

AsmBlockEffects asm_analyze_block(const std::string &nasm_body,
                                  const std::string &arch) {
    AsmBlockEffects res;
    // Marco de pila: seguimos el maximo alcanzado (peor caso), no el neto: un
    // `sub rsp,32; ...; add rsp,32` reserva 32 aunque acabe en 0.
    int64_t cur_frame = 0;
    int64_t max_frame = 0;

    size_t i = 0;
    const std::string &b = nasm_body;
    while (i <= b.size()) {
        size_t eol = b.find('\n', i);
        if (eol == std::string::npos) eol = b.size();
        std::string line = b.substr(i, eol - i);
        i = eol + 1;

        // Comentarios ';' y '//'.
        size_t cpos = line.find(';');
        if (cpos != std::string::npos) line = line.substr(0, cpos);
        size_t slpos = line.find("//");
        if (slpos != std::string::npos) line = line.substr(0, slpos);

        const bool line_has_mem = line.find('[') != std::string::npos;

        auto toks = tokenize_line(line);
        if (toks.empty()) continue;

        // Saltar labels ("name:") y prefijos; detectar el prefijo `lock`
        // (barrera atomica) por el camino.
        size_t ti = 0;
        bool lock_prefix = false;
        while (ti < toks.size()) {
            const std::string &t = toks[ti];
            if (!t.empty() && t.back() == ':') { // label
                ++ti;
                continue;
            }
            const std::string lt = lower(t);
            if (lt == "lock") {
                lock_prefix = true;
                ++ti;
                continue;
            }
            if (lt == "rep" || lt == "repe" || lt == "repz" || lt == "repne" ||
                lt == "repnz" || lt == "bnd") {
                ++ti;
                continue;
            }
            break;
        }
        if (ti >= toks.size()) continue; // linea solo-label/prefijo

        const std::string mnem = lower(toks[ti]);

        if (lock_prefix || line_has_mem) res.touches_mem = true;
        // Prefijo `lock` (x86) o una atomica de arm64 (LL/SC o CAS/RMW/swap) =
        // efecto atomico (barrera).
        if (lock_prefix || es_atomica_arm(mnem)) res.has_atomic = true;
        if (es_rama(mnem)) res.has_branch = true;

        // Marco de pila explicito.
        bool sd_ok = true;
        const int64_t d = stack_delta(toks, ti, arch, sd_ok);
        if (!sd_ok) {
            // El puntero de pila se movio con algo no literal: no acotable.
            res.unknown_mnemonics.push_back(mnem + " sp,<no-literal>");
        } else if (d != 0) {
            cur_frame += d;
            if (cur_frame > max_frame) max_frame = cur_frame;
        }

        // Efectos por-instruccion segun la tabla del arch.  Un mnemonico no
        // tabulado -> a la lista de desconocidos para que el caller de un error
        // claro (disciplina "crecer bajo demanda, error claro siempre").
        const AsmEffects eff = asm_effects_for(mnem, arch);
        if (!eff.known) {
            res.unknown_mnemonics.push_back(mnem);
            continue;
        }
        if (eff.touches_mem) res.touches_mem = true;
        if (eff.touches_flags) res.touches_flags = true;
        if (eff.is_call) res.is_call = true;
    }

    res.explicit_stack_bytes = max_frame;
    return res;
}

} // namespace vex
