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
 * @file asm_analyze.h
 * @brief Resumen de EFECTOS a nivel de BLOQUE de un cuerpo de inline asm.
 *
 * A partir del texto NASM/ARM de un @c asm { } se calcula un modelo de efecto
 * del bloque completo -- si toca memoria, si es atomico, si hace @c call, cuanto
 * marco de pila mueve EXPLICITAMENTE (push/pop/sub rsp) -- para que el compilador
 * sea CONSCIENTE de lo que hace el asm en vez de tratarlo como una caja negra.
 * Lo consume el analizador de huella (para dar @c @stack / @c @alloc /
 * @c @nothrow / @c @pure sobre una funcion con asm) y el servidor de lenguaje
 * (LSP): describir al IDE que hace un bloque de asm.
 *
 * Se apoya en la tabla PLANA por-instruccion @c asm_effects_for (asm_effects.h);
 * NO la duplica, solo la agrega a nivel de bloque.  Modular a proposito: este
 * modulo NO toca el path de inferencia de clobbers ni el ensamblado
 * (Keystone/Capstone); es analisis puro sobre el texto.
 *
 * Un mnemonico DESCONOCIDO para el arch NO se traga -- se acumula en
 * @c unknown_mnemonics para que el caller emita un ERROR CLARO nombrando cual.
 * La tabla de instrucciones reconocidas crece bajo demanda, no se modela todo
 * el juego de instrucciones de golpe.
 *
 * Limite conocido: el marco de pila IMPLICITO de los enlaces @c register()
 * (los spills que decide el asignador de registros del backend) NO es visible
 * en el texto del asm; aqui se cuenta solo el marco EXPLICITO (push/pop/sub
 * rsp/add rsp).  El implicito es una propiedad del codegen (JIT/AOT) y se
 * reportara desde el backend, no desde el texto.
 */

#ifndef VEX_ASM_ANALYZE_H
#define VEX_ASM_ANALYZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

/**
 * @struct AsmBlockEffects
 * @brief Modelo de efecto AGREGADO de un cuerpo de inline asm.
 *
 * Todo lo que el analizador necesita para dar contratos sobre el bloque.  Los
 * campos de efecto son SOUND-conservadores: ante duda (mnemonico desconocido)
 * @c has_unknown queda true y el caller decide (error).
 */
struct AsmBlockEffects {
    bool touches_mem = false;   ///< algun operando @c [...] o instr que toca mem.
    bool has_atomic = false;    ///< prefijo @c lock o instr atomica (barrera).
    bool is_call = false;       ///< @c call / @c syscall alcanzable en el bloque.
    bool touches_flags = false; ///< modifica RFLAGS/condition codes.
    bool has_branch = false;    ///< salto/rama dentro del bloque.
    int64_t explicit_stack_bytes = 0; ///< marco EXPLICITO (push/sub rsp), en bytes.
    std::vector<std::string> unknown_mnemonics; ///< desconocidos -> error claro.

    bool known() const { return unknown_mnemonics.empty(); }
};

/**
 * @brief Analiza un cuerpo NASM y devuelve su modelo de efecto de bloque.
 *
 * Tokeniza linea a linea (descarta comentarios @c ; y @c //, labels @c name:,
 * y separa el prefijo @c lock/@c rep/...), consulta @c asm_effects_for por
 * mnemonico y agrega los efectos.  El marco de pila explicito suma/resta segun
 * @c push/@c pop (8 bytes) y @c sub/@c add sobre @c rsp con inmediato literal.
 *
 * @param nasm_body Cuerpo verbatim del bloque @c asm.
 * @param arch      Arquitectura de destino (@c "x86_64" / @c "arm64" / ...);
 *                  selecciona la tabla de efectos.  Un mnemonico no tabulado
 *                  para ese arch cae en @c unknown_mnemonics.
 * @return Efectos agregados; @c has_unknown()==false si todo se reconocio.
 */
AsmBlockEffects asm_analyze_block(const std::string &nasm_body,
                                  const std::string &arch);

} // namespace vex

#endif // VEX_ASM_ANALYZE_H
