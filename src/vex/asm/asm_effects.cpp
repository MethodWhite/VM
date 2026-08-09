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
 * @file asm_effects.cpp
 * @brief  AS inc.4: implementacion de la inferencia PROPIA de clobbers.
 *
 * Sin dependencias de Keystone/Capstone.  Tabla plana mnemonic->efectos +
 * tokenizador ligero del cuerpo NASM Intel.
 */

#include "vex/asm/asm_effects.h"
#include "vex/parser.h" // get_aot_condcomp_target: el arch del TARGET, no del host

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace vex {

std::string asm_normalize_numbers(const std::string &body) {
    std::string out;
    out.reserve(body.size());
    const size_t n = body.size();
    auto is_id = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    size_t i = 0;
    while (i < n) {
        const char c = body[i];
        /* Placeholder $N (: operando `reg` auto, rellenado por el
         * backend post-regalloc) o referencia a la direccion actual ($): el
         * numero que sigue a '$' NO es un literal a normalizar -> copiar '$' +
         * sus digitos verbatim. */
        if (c == '$') {
            out.push_back('$');
            size_t j = i + 1;
            while (j < n && body[j] >= '0' && body[j] <= '9')
                out.push_back(body[j++]);
            i = j;
            continue;
        }
        /* Identificador (incl. registros r8/r15/xmm0): copiar el token
         * entero -> sus digitos NO son un literal numerico. */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i;
            while (j < n && is_id(body[j]))
                ++j;
            out.append(body, i, j - i);
            i = j;
            continue;
        }
        /* Literal numerico: empieza por digito (char previo no-identificador).
         */
        if (c >= '0' && c <= '9') {
            int base = 10;
            size_t j = i;
            if (c == '0' && i + 1 < n) {
                const char p = body[i + 1];
                if (p == 'x' || p == 'X') {
                    base = 16;
                    j = i + 2;
                } else if (p == 'b' || p == 'B') {
                    base = 2;
                    j = i + 2;
                } else if (p == 'o' || p == 'O') {
                    base = 8;
                    j = i + 2;
                }
            }
            unsigned long long val = 0;
            bool any = false;
            while (j < n) {
                const char d = body[j];
                int dv;
                if (d == '_') {
                    ++j;
                    continue;
                } // separador de digitos
                if (d >= '0' && d <= '9')
                    dv = d - '0';
                else if (d >= 'a' && d <= 'f')
                    dv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F')
                    dv = d - 'A' + 10;
                else
                    break;
                if (dv >= base) break;
                val = val * static_cast<unsigned long long>(base) +
                      static_cast<unsigned long long>(dv);
                any = true;
                ++j;
            }
            if (!any) {
                out.push_back(c);
                ++i;
                continue;
            } // "0" suelto
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx", val);
            out += buf;
            i = j;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Canonicalizacion de registros: UNA por arquitectura.
//
//  Cada arquitectura tiene sus nombres y sus reglas de alias, y no se parecen
//  en nada: mezclarlas en una sola funcion hace que anadir la tercera sea un
//  suplicio y que los nombres de una se cuelen en otra (`x0` no existe en x86,
//  `rax` no existe en ARM, y aceptar el ajeno es un error que hay que dar, no
//  tragarse).  Aqui cada una tiene su funcion, y @ref kArchRegs las asocia con
//  el valor de `arch:` del target.  Anadir RISC-V es anadir su funcion y su
//  fila; no se toca nada de lo demas.
//
//  El criterio comun -- lo unico que comparten -- es que los alias de ANCHO
//  colapsan al registro FISICO: `eax`->`rax` y `w3`->`x3` son la misma idea.
// ---------------------------------------------------------------------------

namespace {

/// Registros de x86-64.  Canonico: el nombre de 64 bits (`rax`, `r10`) y `vN`
/// para el banco vectorial (xmm/ymm/zmm N son la misma pista).
std::string canon_x86_64(const std::string &r) {
    // Registros de proposito general: cada entrada lista todos los alias
    // de ancho que mapean al mismo fisico de 64 bits.
    struct GpEntry {
        const char *canon;
        const char *aliases[6];
    };
    static const GpEntry gp[] = {
        {"rax", {"rax", "eax", "ax", "al", "ah", nullptr}},
        {"rbx", {"rbx", "ebx", "bx", "bl", "bh", nullptr}},
        {"rcx", {"rcx", "ecx", "cx", "cl", "ch", nullptr}},
        {"rdx", {"rdx", "edx", "dx", "dl", "dh", nullptr}},
        {"rsi", {"rsi", "esi", "si", "sil", nullptr}},
        {"rdi", {"rdi", "edi", "di", "dil", nullptr}},
        {"rbp", {"rbp", "ebp", "bp", "bpl", nullptr}},
        {"rsp", {"rsp", "esp", "sp", "spl", nullptr}},
    };
    for (const auto &e : gp) {
        for (const char *const *a = e.aliases; *a; ++a) {
            if (r == *a) return e.canon;
        }
    }
    // r8..r15 con sufijos d/w/b (32/16/8 bits) -> canonico rN.
    if (r.size() >= 2 && r[0] == 'r' && std::isdigit((unsigned char)r[1])) {
        std::string num;
        size_t i = 1;
        while (i < r.size() && std::isdigit((unsigned char)r[i]))
            num.push_back(r[i++]);
        std::string suf = r.substr(i);
        if (!num.empty() &&
            (suf.empty() || suf == "d" || suf == "w" || suf == "b")) {
            int n = std::atoi(num.c_str());
            if (n >= 8 && n <= 15) return "r" + num;
        }
    }
    // Vectoriales xmm/ymm/zmm N (0..31) -> canonico vN (mismo banco).
    for (const char *pfx : {"xmm", "ymm", "zmm"}) {
        const size_t plen = std::strlen(pfx);
        if (r.size() > plen && r.compare(0, plen, pfx) == 0) {
            std::string num = r.substr(plen);
            bool all_dig = !num.empty();
            for (char c : num) {
                if (!std::isdigit((unsigned char)c)) {
                    all_dig = false;
                    break;
                }
            }
            if (all_dig) {
                int n = std::atoi(num.c_str());
                if (n >= 0 && n <= 31) return "v" + num;
            }
        }
    }
    return std::string();
}

/// Todos los digitos y no vacio.
bool solo_digitos(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

/// Registros de AArch64.  Canonico: el nombre de 64 bits (`x3`) y `vN` para el
/// banco SIMD/FP.  Nada que ver con los de x86: aqui `w3` es la mitad baja de
/// `x3` (como `eax` de `rax`) y `b0`/`h0`/`s0`/`d0`/`q0`/`v0` son la misma
/// pista SIMD vista con distinto ancho.
std::string canon_arm64(const std::string &r) {
    if (r == "xzr" || r == "wzr") return "xzr";              // registro cero
    if (r == "sp" || r == "wsp") return "sp";                // stack pointer
    if (r == "lr" || r == "x30" || r == "w30") return "x30"; // link register
    if (r == "fp" || r == "x29" || r == "w29") return "x29"; // frame pointer

    // x0..x30 / w0..w30 -> xN.
    if (r.size() >= 2 && (r[0] == 'x' || r[0] == 'w')) {
        const std::string num = r.substr(1);
        if (solo_digitos(num)) {
            const int n = std::atoi(num.c_str());
            if (n >= 0 && n <= 30) return "x" + num;
        }
    }
    // Banco SIMD/FP: b/h/s/d/q/v N (0..31) -> vN.  Se admite la forma con lane
    // (`v0.4s`), que nombra al mismo fisico.
    if (r.size() >= 2 && (r[0] == 'b' || r[0] == 'h' || r[0] == 's' ||
                          r[0] == 'd' || r[0] == 'q' || r[0] == 'v')) {
        std::string num = r.substr(1);
        const size_t punto = num.find('.');
        if (punto != std::string::npos) num = num.substr(0, punto);
        if (solo_digitos(num)) {
            const int n = std::atoi(num.c_str());
            if (n >= 0 && n <= 31) return "v" + num;
        }
    }
    return std::string();
}

/// Que canonicalizador usa cada valor de `arch:`.  Anadir una arquitectura es
/// anadir su funcion y su fila aqui.
struct ArchRegs {
    const char *arch;
    std::string (*canon)(const std::string &);
};
const ArchRegs kArchRegs[] = {
    {"x86_64", &canon_x86_64},
    {"x86", &canon_x86_64}, // mismo banco (los de 64 bits sobran, y el
                            // ensamblador ya los rechaza por su cuenta)
    {"arm64", &canon_arm64},
};

/// La arquitectura del host, para cuando no hay override de target.
const char *arch_host() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "x86_64";
#endif
}

} // namespace

std::string asm_canonical_reg(const std::string &raw) {
    std::string r;
    r.reserve(raw.size());
    for (char c : raw)
        r.push_back((char)std::tolower((unsigned char)c));

    // Se despacha por el arch del build actual.  (La rama local no tiene
    // get_aot_condcomp_target; el cross-compile por @Target es un futuro
    // port del AOT de Desmon.  Con el arch del host, register("x0") en
    // un build arm64 funciona y en x86 se rechaza, que es lo correcto.)
    std::string arch = arch_host();
    for (const auto &a : kArchRegs)
        if (arch == a.arch) return a.canon(r);
    return std::string();
}

// -----------------------------------------------------------------------
// Tabla plana mnemonic -> AsmEffects.  Subset comun, extensible.  Los
// registros van en forma canonica.  Construida una vez (lazy) en un
// unordered_map; el lookup es compile-time (no hot path runtime).
// -----------------------------------------------------------------------
namespace {

/// Tipo de una tabla de efectos por-mnemonico (en forma canonica-minuscula).
using EffTable = std::unordered_map<std::string, AsmEffects>;

/// Tabla de efectos x86 (16/32/64 comparten mnemonicos; el efecto de @c add /
/// @c mov / @c cmp no cambia con el ancho).  Lazy-init, se construye una vez.
const EffTable &x86_effects_table() {
    static const EffTable table = [] {
        EffTable t;
        auto add = [&t](const char *m, AsmEffects e) {
            e.known = true;
            t[m] = std::move(e);
        };
        // Helpers de construccion.  wmask = bitmask de operandos escritos
        // (bit0=op1, bit1=op2, ...); un `true`/`false` viejo cuenta como 0x1/0x0
        // (escribe/no el 1er operando).
        auto E = [](std::initializer_list<const char *> wr, uint8_t wmask,
                    bool mem, bool flags, bool call = false,
                    std::initializer_list<const char *> rd = {}) {
            AsmEffects e;
            for (const char *w : wr)
                e.implicit_write.emplace_back(w);
            for (const char *r : rd)
                e.implicit_read.emplace_back(r);
            e.operand_write_mask = wmask;
            e.touches_mem = mem;
            e.touches_flags = flags;
            e.is_call = call;
            return e;
        };
        // --- Instrucciones con efectos IMPLICITOS sobre registros ---
        add("rdtsc", E({"rax", "rdx"}, false, false, false));
        add("rdtscp", E({"rax", "rdx", "rcx"}, false, false, false));
        add("cpuid", E({"rax", "rbx", "rcx", "rdx"}, false, false, false));
        add("rdpmc", E({"rax", "rdx"}, false, false, false));
        add("rdrand", E({}, true, false, true)); // escribe dest + CF
        add("rdseed", E({}, true, false, true));
        // mul/imul 1-operando + div/idiv: rdx:rax.
        add("mul", E({"rax", "rdx"}, false, false, true));
        add("imul",
            E({"rax", "rdx"}, false, false, true)); // forma 1-op (conservador)
        add("div", E({"rax", "rdx"}, false, false, true));
        add("idiv", E({"rax", "rdx"}, false, false, true));
        add("cqo", E({"rdx"}, false, false, false));
        add("cdq", E({"rdx"}, false, false, false));
        // syscall (Linux x64 + Windows x64 NT): escribe RAX (valor de retorno) +
        // clobber rcx, r11 + caller-saved via is_call.  RAX en implicit_write
        // hace que un param `register("rax")` leido tras el asm (read-back del
        // resultado) se clasifique INOUT (el asm lo define).  La MISMA
        // instruccion cubre Linux y Windows x64 (el numero de servicio y los
        // arg-regs son convencion del usuario via register(), no del opcode).
        // LEE el numero de servicio (RAX) + los args.  Conservador: la union de
        // las convenciones Linux (RDI/RSI/RDX/R10/R8/R9) y Windows NT (R10/RDX/
        // R8/R9) -- asi un `register("rdi")`/`register("r10")` vive HASTA el
        // syscall y el arg se coloca en su registro (sin esto el DCE lo borraba).
        add("syscall", E({"rax", "rcx", "r11"}, false, true, true, /*call=*/true,
                         {"rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"}));
        add("sysenter", E({"rax"}, false, true, true, true,
                          {"rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"}));
        // int (Linux x86-32 `int 0x80`): escribe EAX con el valor de retorno y
        // LEE EAX (num) + EBX/ECX/EDX/ESI/EDI/EBP (args).  Canonicos (rax/rbx/...).
        add("int", E({"rax"}, false, true, true, /*call=*/true,
                     {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp"}));
        // call/ret: call clobbera caller-saved (is_call).
        add("call", E({}, false, true, true, /*call=*/true));
        // --- Aritmetica/bitwise que escribe el 1er operando + flags ---
        add("add", E({}, true, false, true));
        add("sub", E({}, true, false, true));
        add("adc", E({}, true, false, true));
        add("sbb", E({}, true, false, true));
        add("and", E({}, true, false, true));
        add("or", E({}, true, false, true));
        add("xor", E({}, true, false, true));
        add("neg", E({}, true, false, true));
        add("inc", E({}, true, false, true)); // no CF, pero conservador
        add("dec", E({}, true, false, true));
        add("shl", E({}, true, false, true));
        add("shr", E({}, true, false, true));
        add("sar", E({}, true, false, true));
        add("rol", E({}, true, false, true));
        add("ror", E({}, true, false, true));
        add("rcl", E({}, true, false, true));
        add("rcr", E({}, true, false, true));
        add("popcnt", E({}, true, false, true));
        add("lzcnt", E({}, true, false, true));
        add("tzcnt", E({}, true, false, true));
        add("bsf", E({}, true, false, true));
        add("bsr", E({}, true, false, true));
        add("bswap", E({}, true, false, false));
        // --- Movimientos / direcciones (escriben 1er operando, sin flags) ---
        add("mov", E({}, true, false, false));
        add("movzx", E({}, true, false, false));
        add("movsx", E({}, true, false, false));
        add("movsxd", E({}, true, false, false));
        add("lea", E({}, true, false, false));
        add("xchg", E({}, 0x3, false, false)); // escribe AMBOS operandos
        // --- Atomicas RMW x86 (siempre con prefijo lock salvo xchg): tocan
        //     memoria + flags; cmpxchg compara/escribe rax.  cmpxchg16b usa
        //     rdx:rax (esperado) y rcx:rbx (deseado) -> DWCAS lock-free real. ---
        add("cmpxchg", E({"rax"}, 0x1, true, true)); // dest(op1) + rax + mem
        add("cmpxchg8b", E({"rax", "rdx"}, false, true, true));
        add("cmpxchg16b", E({"rax", "rdx"}, false, true, true));
        add("xadd", E({}, true, true, true));           // 1er op + mem + flags
        add("cmov", E({}, true, false, false));
        add("not", E({}, true, false, false)); // not no toca flags
        add("set", E({}, true, false, false)); // setcc: escribe 1er op (byte)
        // --- Comparaciones / test: solo flags ---
        add("cmp", E({}, false, false, true));
        add("test", E({}, false, false, true));
        // --- Pila: tocan memoria + rsp ---
        add("push", E({"rsp"}, false, true, false));
        add("pop", E({"rsp"}, true, true, false));
        // --- Control de flujo / no-ops: sin clobbers ---
        add("nop", E({}, false, false, false));
        add("ret", E({}, false, true, false));
        add("jmp", E({}, false, false, false));
        add("leave", E({"rsp", "rbp"}, false, true, false));
        // saltos condicionales: solo leen flags.
        for (const char *j : {"je",  "jne", "jz",  "jnz", "jg",  "jge", "jl",
                              "jle", "ja",  "jae", "jb",  "jbe", "jo",  "jno",
                              "js",  "jns", "jc",  "jnc", "jp",  "jnp"}) {
            add(j, E({}, false, false, false));
        }
        return t;
    }();
    return table;
}

/// Tabla de efectos AArch64 (arm64).  A diferencia de x86, la aritmetica base
/// NO toca flags (solo las formas con sufijo @c s: @c adds/@c subs/@c ands);
/// @c cmp/@c cmn/@c tst comparan sin escribir registro.  Las cargas/almacenes
/// LL/SC (@c ldaxr/@c stlxr...) y las CAS/RMW atomicas (@c cas*/@c ldadd*/
/// @c swp*) tocan memoria; el modulo de bloque las marca ademas como atomicas.
const EffTable &arm64_effects_table() {
    static const EffTable table = [] {
        EffTable t;
        auto add = [&t](const char *m, AsmEffects e) {
            e.known = true;
            t[m] = std::move(e);
        };
        // wr=implicit_write, wmask=bitmask de operandos escritos, mem, flags,
        // call (un `true`/`false` viejo = 0x1/0x0, escribe/no el 1er operando).
        auto E = [](std::initializer_list<const char *> wr, uint8_t wmask,
                    bool mem, bool flags, bool call = false,
                    std::initializer_list<const char *> rd = {}) {
            AsmEffects e;
            for (const char *w : wr)
                e.implicit_write.emplace_back(w);
            for (const char *r : rd)
                e.implicit_read.emplace_back(r);
            e.operand_write_mask = wmask;
            e.touches_mem = mem;
            e.touches_flags = flags;
            e.is_call = call;
            return e;
        };
        // --- Aritmetica/logica (escriben 1er operando; NO flags salvo `s`) ---
        for (const char *m : {"add", "sub", "mul", "madd", "msub", "and", "orr",
                              "eor", "bic", "orn", "eon", "lsl", "lsr", "asr",
                              "ror", "neg", "mvn", "udiv", "sdiv", "smull",
                              "umull", "sxtw", "uxtw", "sxth", "uxth", "sxtb",
                              "uxtb"})
            add(m, E({}, true, false, false));
        // Formas que SI actualizan flags (sufijo `s`).
        for (const char *m : {"adds", "subs", "ands", "bics", "negs"})
            add(m, E({}, true, false, true));
        // cmp/cmn/tst: comparan (solo flags, sin escribir registro).
        add("cmp", E({}, false, false, true));
        add("cmn", E({}, false, false, true));
        add("tst", E({}, false, false, true));
        // --- Movimientos / carga de inmediato / direccion ---
        for (const char *m : {"mov", "movz", "movk", "movn", "adr", "adrp",
                              "fmov"})
            add(m, E({}, true, false, false));
        // --- Seleccion condicional (leen flags, escriben 1er operando) ---
        for (const char *m : {"csel", "cset", "csetm", "csinc", "csinv",
                              "csneg", "cinc", "cinv", "cneg", "ccmp", "ccmn"})
            add(m, E({}, true, false, false));
        // --- Bit / rev / bitfield (escriben 1er operando) ---
        for (const char *m : {"clz", "cls", "rbit", "rev", "rev16", "rev32",
                              "ubfx", "sbfx", "ubfm", "sbfm", "bfi", "bfxil",
                              "extr"})
            add(m, E({}, true, false, false));
        // --- Cargas: escriben 1er operando + memoria ---
        for (const char *m : {"ldr", "ldrb", "ldrh", "ldrsw", "ldrsb", "ldrsh",
                              "ldur", "ldp"})
            add(m, E({}, true, true, false));
        // --- Almacenes: no escriben registro, tocan memoria ---
        for (const char *m : {"str", "strb", "strh", "stur", "stp"})
            add(m, E({}, false, true, false));
        // --- LL/SC atomicas: load-acquire escribe 1er op; store-cond escribe
        //     el registro de estado (1er op) -- ambas tocan memoria. ---
        for (const char *m : {"ldaxr", "ldxr", "ldar", "ldaxrb", "ldxrb",
                              "ldarb", "ldaxrh", "ldxrh", "ldarh"})
            add(m, E({}, 0x1, true, false));
        // ldaxp/ldxp cargan un PAR -> escriben op1 Y op2.
        for (const char *m : {"ldaxp", "ldxp"})
            add(m, E({}, 0x3, true, false));
        for (const char *m : {"stlxr", "stxr", "stlr", "stlxrb", "stxrb",
                              "stlrb", "stlxrh", "stxrh", "stlrh", "stlxp",
                              "stxp"})
            add(m, E({}, 0x1, true, false)); // status en op1
        // --- CAS / RMW atomicas (armv8.1): escriben el destino + memoria ---
        for (const char *m : {"cas", "casa", "casl", "casal", "casb", "casab",
                              "caslb", "casalb", "cash", "casah", "caslh",
                              "casalh", "swp", "swpa", "swpl", "swpal", "ldadd",
                              "ldadda", "ldaddl", "ldaddal", "ldset", "ldseta",
                              "ldsetl", "ldsetal", "ldclr", "ldclra", "ldclrl",
                              "ldclral", "ldeor", "ldeora", "ldeorl", "ldeoral"})
            add(m, E({}, 0x1, true, false));
        // casp* comparan/escriben un PAR de registros (op1 Y op2).
        for (const char *m : {"casp", "caspa", "caspl", "caspal"})
            add(m, E({}, 0x3, true, false));
        // --- Barreras de memoria (efecto de orden; tocan memoria conserv.) ---
        for (const char *m : {"dmb", "dsb", "isb"})
            add(m, E({}, false, true, false));
        // --- Control de flujo ---
        add("ret", E({}, false, true, false));
        add("br", E({}, false, false, false));
        add("blr", E({}, false, true, true)); // llamada indirecta
        add("bl", E({}, false, true, true));  // llamada directa
        add("b", E({}, false, false, false));
        for (const char *m : {"cbz", "cbnz", "tbz", "tbnz"})
            add(m, E({}, false, false, false));
        // b.CC (condicionales) -> las reconoce el postfijo de asm_effects_for.
        // --- No-ops / hint ---
        for (const char *m : {"nop", "yield", "wfe", "wfi", "sev", "sevl",
                              "hint"})
            add(m, E({}, false, false, false));
        // svc #0 (ARM64/Linux syscall): LEE el numero de servicio en X8 y los
        // argumentos en X0..X7; ESCRIBE el resultado en X0.  Sin declarar los
        // reads, un `register("x0")` no viviria hasta el svc y el arg no se
        // colocaria en su registro (mismo analisis que `syscall`/`int` en x86).
        add("svc", E({"x0"}, false, true, true, /*call=*/true,
                     {"x8", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"}));
        return t;
    }();
    return table;
}

/// @c true si @p arch es una variante x86 (16/32/64 comparten tabla).
bool es_x86(const std::string &arch) {
    return arch.rfind("x86", 0) == 0 || arch.empty();
}

} // namespace

AsmEffects asm_effects_for(const std::string &mnemonic,
                           const std::string &arch) {
    std::string m;
    m.reserve(mnemonic.size());
    for (char c : mnemonic)
        m.push_back((char)std::tolower((unsigned char)c));

    const bool x86 = es_x86(arch);
    const EffTable &table = x86 ? x86_effects_table() : arm64_effects_table();
    auto it = table.find(m);
    if (it != table.end()) return it->second;

    if (x86) {
        // setcc (sete/setne/...) y cmovcc se reconocen por prefijo: escriben
        // el 1er operando.
        if (m.rfind("set", 0) == 0 && m.size() > 3) {
            AsmEffects e;
            e.operand_write_mask = 0x1;
            e.known = true;
            return e;
        }
        if (m.rfind("cmov", 0) == 0 && m.size() > 4) {
            AsmEffects e;
            e.operand_write_mask = 0x1;
            e.known = true;
            return e;
        }
    } else {
        // arm64: b.CC (b.eq/b.ne/...) es una rama que solo lee flags.
        if (m.rfind("b.", 0) == 0 && m.size() > 2) {
            AsmEffects e;
            e.known = true;
            return e;
        }
    }
    AsmEffects unknown; // known=false
    return unknown;
}

AsmEffects asm_effects_for(const std::string &mnemonic) {
    return asm_effects_for(mnemonic, "x86_64");
}

namespace {
// Convierte un canonico (rax.., vN) al nombre que GCC espera en la
// clobber-list.  GP: tal cual (rax..r15).  Vectorial vN -> xmmN.
std::string canon_to_gcc(const std::string &canon) {
    if (!canon.empty() && canon[0] == 'v' && canon.size() > 1 &&
        std::isdigit((unsigned char)canon[1])) {
        return "xmm" + canon.substr(1);
    }
    return canon;
}

// Trocea una linea en tokens separados por espacios/comas/tabs.  Los
// corchetes y su contenido se conservan como parte del token (para
// detectar [mem]); aqui solo necesitamos el 1er token (mnemonico) y
// saber si el resto contiene un registro como primer operando.
std::vector<std::string> tokenize_line(const std::string &line) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == ',') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}
} // namespace

AsmInferResult asm_infer_clobbers(const std::string &nasm_body,
                                  const std::vector<std::string> &bound_canon) {
    AsmInferResult res;
    // Set de regs ligados (canonicos) a EXCLUIR de los clobbers.
    std::unordered_set<std::string> bound(bound_canon.begin(),
                                          bound_canon.end());
    // Acumulador canonico de clobbers (despues -> GCC names + filtrado).
    std::unordered_set<std::string> clob;

    // Caller-saved conservador (union SysV + Win64) para call/syscall.
    static const char *caller_saved[] = {"rax", "rcx", "rdx", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11"};

    // Procesar linea a linea.
    size_t i = 0;
    const std::string &b = nasm_body;
    while (i <= b.size()) {
        // Extraer una linea [start, eol).
        size_t eol = b.find('\n', i);
        if (eol == std::string::npos) eol = b.size();
        std::string line = b.substr(i, eol - i);
        i = eol + 1;

        // Quitar comentarios ';' y '//'.
        size_t cpos = line.find(';');
        if (cpos != std::string::npos) line = line.substr(0, cpos);
        size_t slpos = line.find("//");
        if (slpos != std::string::npos) line = line.substr(0, slpos);

        // Detectar memoria: cualquier '[' en operandos.
        const bool line_has_mem = line.find('[') != std::string::npos;

        auto toks = tokenize_line(line);
        if (toks.empty()) continue;

        // Saltar labels iniciales ("name:" como token aislado) y prefijos.
        size_t ti = 0;
        while (ti < toks.size()) {
            const std::string &t = toks[ti];
            // Label: token que termina en ':'.
            if (!t.empty() && t.back() == ':') {
                ++ti;
                continue;
            }
            // Prefijos de instruccion.
            std::string lt;
            for (char c : t)
                lt.push_back((char)std::tolower((unsigned char)c));
            if (lt == "lock" || lt == "rep" || lt == "repe" || lt == "repz" ||
                lt == "repne" || lt == "repnz" || lt == "bnd") {
                ++ti;
                continue;
            }
            break;
        }
        if (ti >= toks.size()) continue; // linea solo-label/prefijo

        const std::string mnem = toks[ti];
        const AsmEffects eff = asm_effects_for(mnem);

        if (!eff.known) {
            res.unknown_mnemonics.push_back(mnem);
            // Conservador: marcar memoria + flags para no perder un efecto.
            res.clobber_memory = true;
            res.clobber_flags = true;
            continue;
        }

        // Registros escritos implicitamente.
        for (const auto &w : eff.implicit_write)
            clob.insert(w);

        // Operandos escritos segun el bitmask: bit i -> operando i+1 (los
        // operandos son los tokens tras el mnemonico).  Si el operando es un
        // registro, es un clobber.  Generaliza el "1er operando" a xchg (ambos),
        // casp/ldxp (pares), etc.
        for (int bit = 0; bit < 8; ++bit) {
            if (!(eff.operand_write_mask & (1u << bit))) continue;
            const size_t opi = ti + 1 + static_cast<size_t>(bit);
            if (opi >= toks.size()) break;
            const std::string canon = asm_canonical_reg(toks[opi]);
            if (!canon.empty()) clob.insert(canon);
        }

        // Memoria: implicita del mnemonico o '[' en la linea.
        if (eff.touches_mem || line_has_mem) res.clobber_memory = true;
        if (eff.touches_flags) res.clobber_flags = true;

        // call/syscall: clobber conservador de TODO el caller-saved.
        if (eff.is_call) {
            for (const char *cs : caller_saved)
                clob.insert(cs);
        }
    }

    // Excluir los regs ligados por register() (son operandos) + rsp/rbp
    // (nunca se pueden clobrear en GCC) y convertir a nombres GCC.
    for (const auto &c : clob) {
        if (bound.count(c)) continue;
        if (c == "rsp" || c == "rbp") continue;
        res.clobber_regs.push_back(canon_to_gcc(c));
    }
    std::sort(res.clobber_regs.begin(), res.clobber_regs.end());
    return res;
}

} // namespace vex
