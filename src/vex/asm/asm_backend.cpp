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
 * @file asm_backend.cpp
 * @brief  AS inc.4b: definicion del puntero global @c g_asm_backend.
 *
 * El frontend Vesta (vx_lib) define el puntero a @c nullptr.  El ejecutable
 * que enlaza Keystone (target @c vm) registra una @c KeystoneAsmBackend en el
 * arranque.  Asi vx_lib NO depende de Keystone.
 */

#include "vex/asm/asm_backend.h"

#include "vex/parser.h" // get_aot_condcomp_target: el arch del TARGET, no del host

namespace vex {
AsmBackend *g_asm_backend = nullptr;

AsmArch asm_arch_for_target(int bits) {
    // Sin override -> el host.  El compilador nativo hoy es x86-64; ARM se
    // ensambla cuando una variante @Target lo pide (o cuando el driver AOT
    // genera para ese target).  (La rama local no tiene
    // get_aot_condcomp_target.)
    std::string arch;
#if defined(__aarch64__) || defined(_M_ARM64)
    arch = "arm64";
#else
    arch = "x86_64";
#endif
    if (arch == "arm64") return AsmArch::ARM64;
    if (arch == "arm32" || arch == "arm") return AsmArch::ARM32;
    // x86: aqui SI mandan los bits (@bits: modo real/protegido/largo).
    if (bits == 16) return AsmArch::X86_16;
    if (bits == 32) return AsmArch::X86_32;
    return AsmArch::X86_64;
}
}
