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
 * @file asm_diag.h
 * @brief Diagnosticos estructurales de un bloque de inline asm derivados de su
 *        CFG (ver vx/asm_cfg.h).
 *
 * Sobre el grafo de flujo reconstruido detecta, de forma SoLIDA (sin falsos
 * positivos: solo avisa cuando la propiedad es cierta por alcanzabilidad):
 *   - CoDIGO MUERTO: instrucciones de un bloque que no es alcanzable desde la
 *     entrada del bloque asm.
 *   - SALTO NO RESUELTO: rama a una etiqueta que no se define en el bloque.
 *   - BUCLE SIN SALIDA: una region alcanzable desde la que no se puede llegar a
 *     ninguna salida (ret o caida al final) -> bucle infinito.
 *
 * NO analiza registros/flags todavia (eso es el dataflow, un paso aparte): estos
 * tres diagnosticos salen solo de la topologia del CFG y por eso son seguros.
 * La cadena de codigos es estable (VXA0xx) para el catalogo de diagnosticos.
 */

#ifndef VEX_ASM_DIAG_H
#define VEX_ASM_DIAG_H

#include <cstdint>
#include <string>
#include <vector>

#include "vex/asm/asm_cfg.h"
#include "vex/asm/instr_db.h"

namespace vex {

/// Severidad de un diagnostico del asm.
enum class AsmDiagSeverity : uint8_t { Info, Warning, Error };

/// Un diagnostico estructural del bloque de asm.  Lleva el CODIGO estable + los
/// ARGS (datos); el texto lo resuelve el catalogo multi-idioma al imprimir (ver
/// vx/diag_catalog.h).  No lleva mensaje pre-formateado.
struct AsmDiag {
    AsmDiagSeverity severity = AsmDiagSeverity::Warning;
    uint32_t line_no = 0;    ///< linea fisica dentro del bloque (1-based).
    std::string code;        ///< codigo estable del catalogo (p.ej. "VXA001").
    std::vector<std::string> args; ///< valores para los placeholders {0},{1},...
};

/**
 * @brief Diagnostica un CFG de asm ya construido.
 * @param cfg CFG (de @ref build_asm_cfg).
 * @return Lista de diagnosticos (vacia si el bloque es estructuralmente limpio).
 */
std::vector<AsmDiag> asm_diagnose_cfg(const AsmCfg &cfg);

/**
 * @brief Atajo: construye el CFG de @p body y lo diagnostica.
 */
std::vector<AsmDiag> asm_diagnose(instr_db::Isa isa, const std::string &body);

/**
 * @brief Detecta LECTURAS DE REGISTRO SIN INICIALIZAR mediante un dataflow
 *        MUST-undefined sobre el CFG.
 *
 * Un registro se marca sin inicializar (VXA004) solo si en TODOS los caminos
 * desde la entrada hasta la lectura no fue escrito por ninguna instruccion
 * MODELADA y no aparecio ninguna instruccion no-modelada (que podria haberlo
 * escrito) -> es SoLIDO (cero falsos positivos; una instruccion desconocida
 * suprime el aviso, conservador).
 *
 * @param cfg        CFG del bloque (de @ref build_asm_cfg).
 * @param isa        ISA (para la semantica por instruccion).
 * @param defined_in Registros CANoNICOS ya definidos a la entrada del bloque
 *                   (los ligados por @c register() y las entradas ABI).  Sin
 *                   este conjunto CUALQUIER primera lectura seria un falso
 *                   positivo, por eso es un parametro obligatorio.
 * @param ua_id      Microarq para la semantica (las lecturas/escrituras no
 *                   dependen de ella; solo se usa para resolver la forma).
 * @return Diagnosticos VXA004 (registro sin inicializar) y VXA005 (flags leidas
 *         sin una comparacion/operacion previa: p.ej. `jz` antes de `cmp`), uno
 *         por (linea, registro/flags).
 *
 * Las flags (RFLAGS / NZCV) se modelan como un unico valor: una rama condicional
 * las LEE, y `cmp`/`add`/`sub`/`test`/... las ESCRIBEN.  Solo se analiza en x86 y
 * arm64 (RISC-V no tiene registro de flags; sus ramas comparan registros).
 */
std::vector<AsmDiag>
asm_diagnose_uninit(const AsmCfg &cfg, instr_db::Isa isa,
                    const std::vector<std::string> &defined_in, uint32_t ua_id);

} // namespace vex

#endif // VEX_ASM_DIAG_H
