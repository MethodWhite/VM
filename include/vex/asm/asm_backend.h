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
 * @file asm_backend.h
 * @brief  AS inc.4b: interfaz @c AsmBackend para ENSAMBLAR inline asm.
 *
 * EJE ORTOGONAL a la inferencia de clobbers (asm_effects.h).  @c AsmBackend
 * cubre SOLO el ensamblado texto->bytes (lo "pesado").  La inferencia es
 * propia y NO consume esta interfaz.
 *
 * Decision de diseno (cerrada): ningun fichero del frontend Vesta incluye
 * @c keystone.h directamente.  El unico contacto vive en UN solo .cpp
 * (@c src/jit/keystone_asm_backend.cpp) detras de esta interfaz pura.  El
 * ejecutable que enlaza Keystone (target @c vm) registra una instancia en
 * @c g_asm_backend; el frontend la usa via el puntero (sin dependencia de
 * Keystone).  Swap futuro a un encoder hand-rolled = otra impl de la interfaz.
 *
 * Hoy (inc.4b) se usa para VALIDAR la sintaxis del body en compile-time
 * (errores con la linea Vesta).  En inc.5 (JIT) el mismo @c assemble produce
 * los BYTES que van al code-cache.
 */

#ifndef VX_ASM_BACKEND_H
#define VX_ASM_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

/// Arquitectura destino del ensamblado.
enum class AsmArch { X86_64, X86_32, X86_16, ARM64, ARM32 };

/// @brief El @ref AsmArch de un bloque de inline asm, segun el TARGET.
///
/// El inline asm se ensambla para la arquitectura del TARGET, no la del host:
/// un `asm { ... }` dentro de una variante `@Target("arch:arm64")` es codigo
/// ARM y hay que ensamblarlo en modo ARM aunque el build corra en x86.  Antes
/// se elegia solo por los bits (`@bits(16|32|64)`) y salia SIEMPRE x86, con lo
/// que las variantes arm64 daban "Invalid mnemonic" en Keystone: no se habian
/// ensamblado nunca.
///
/// @param bits el ancho de `@bits(...)`, que solo tiene sentido en x86 (modo
///        real / protegido / largo).  En ARM se ignora: la anchura del modo la
///        fija el propio ISA, no una directiva.
///
/// Un solo sitio traduce `arch:` -> @ref AsmArch, para que anadir una
/// arquitectura sea tocar aqui y no cada punto que ensambla.
AsmArch asm_arch_for_target(int bits);

/// Resultado de ensamblar un fragmento de asm.
struct AsmAssembleResult {
    bool ok = false;            ///< true si ensamblo sin errores
    std::vector<uint8_t> bytes; ///< bytes maquina (inc.5 JIT)
    std::string error;          ///< mensaje si @c ok==false
    size_t error_offset = 0;    ///< offset aprox en el body del fallo
    /// CONTRATO opcional (solo-inspeccion): offset en bytes de CADA instruccion
    /// emitida, en ORDEN de fuente (insn_offsets[i] = offset de la i-esima
    /// instruccion).  Permite a NUESTRO codigo mapear etiquetas/lineas a
    /// offsets sin acoplar la inspeccion al backend concreto.  Un backend que no
    /// lo soporte lo deja vacio (la inspeccion degrada sin etiquetas de asm).
    std::vector<uint32_t> insn_offsets;

    /// Reloc de un SIMBOLO referenciado desde el asm (`jmp [sym]`, `mov rax,
    /// sym`, ...).  El backend lo resuelve a un sentinela, desensambla para
    /// localizar el campo, y deja aqui (offset del campo en @c bytes, tamano,
    /// nombre del simbolo, tipo).  El consumidor (AOT) lo emite a la tabla de
    /// relocs del .o para que el linker lo parchee con la direccion real.
    /// Tipo de la referencia, segun la FORMA de la instruccion (lo decide el
    /// usuario en el asm: jmp/call directo, indirecto, mov, lea, ...).
    enum class SymRefKind : uint8_t {
        BranchRel32, ///< `jmp sym` / `call sym` directos: rel32 a una FUNCION.
        DataRel32,   ///< `jmp [sym]` / `lea reg,[rip+sym]`: rip-rel a un dato.
        Abs64,       ///< `mov reg, sym`: direccion absoluta 64-bit (imm64).
        Abs32,       ///< `push sym` / `mov r32, sym`: absoluto 32-bit (imm32).
    };
    struct SymRef {
        uint32_t offset = 0;  ///< offset del campo dentro de @c bytes
        uint8_t size = 0;     ///< 4 u 8
        /// Bytes que siguen al campo DENTRO de la misma instruccion (p.ej. el
        /// imm32 de `mov [rip+disp32], imm32`).  Solo relevante para rip-rel
        /// (DataRel32): el disp32 se mide desde el FIN de instruccion, no desde
        /// el fin del campo disp.  El consumidor resta este valor al addend.
        uint8_t pcrel_trailing = 0;
        SymRefKind kind = SymRefKind::DataRel32;
        std::string symbol;   ///< nombre del simbolo Vesta
    };
    std::vector<SymRef> sym_refs;
};

/**
 * @brief Interfaz pura: ensambla texto NASM Intel a bytes maquina.
 *
 * Impl1 = @c KeystoneAsmBackend (src/jit/keystone_asm_backend.cpp).  Impl2
 * futuro = encoder hand-rolled del JIT.  La inferencia de clobbers NO usa
 * esta interfaz (vive en asm_effects.{h,cpp}).
 */
struct AsmBackend {
    virtual ~AsmBackend() = default;
    /**
     * @brief Ensambla @p nasm (sintaxis Intel/NASM) para @p arch.
     * @return Bytes + ok, o ok=false + error si la sintaxis es invalida.
     */
    virtual AsmAssembleResult assemble(const std::string &nasm,
                                       AsmArch arch) = 0;
};

/**
 * @brief Backend de ensamblado activo (registrado por el ejecutable que
 *        enlaza Keystone).  @c nullptr si no hay backend disponible (e.g.
 *        tests que invocan el compilador sin pasar por main.cpp); en ese
 *        caso la validacion de sintaxis en compile-time se omite y GCC
 *        valida al compilar el .c (port-C).
 */
extern AsmBackend *g_asm_backend;

} // namespace vex

#endif // VX_ASM_BACKEND_H
