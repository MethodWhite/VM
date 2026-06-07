/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/regalloc_rewrite.h
 * @brief Rewrite de MachineIR vreg -> MachineIR fisica (Phase D.7, commit 4a).
 *        Ver doc/REGALLOC.md.
 *
 * Toma una @c MFunction en forma de registros virtuales (3-operandos,
 * pre-legalization) y la asignacion del linear-scan, y produce una NUEVA
 * @c MFunction donde todos los operandos son fisicos (REG/MEM/IMM), lista
 * para el @c X86Encoder existente.  Hace:
 *
 *   1. Sustitucion VREG -> registro fisico (si esta en reg) o acceso a su
 *      spill slot @c [rbp+off] (si esta spilled).
 *   2. Two-address legalization: el encoder es de DOS operandos
 *      (@c "add dst, src" -> dst = dst OP src); la forma vreg es de TRES
 *      (@c "add dst, src1, src2" -> dst = src1 OP src2).  Se inserta el
 *      @c "mov dst, src1" y se resuelve la anti-dependencia cuando dst y
 *      src2 caen en el mismo fisico (conmutativo: reordenar; no conmutativo:
 *      usar un scratch).
 *   3. Insercion de load/store de spills (el encoder soporta ALU reg/mem, asi
 *      que un src spilled se lee directo; un dst spilled se materializa en un
 *      scratch y se almacena).
 *   4. Prologue/epilogue: push/pop de los callee-saved usados + reserva del
 *      frame para los spill slots.
 *
 * Cobertura commit 4a: MOV, ALU binario (ADD/SUB/AND/OR/XOR/IMUL), CMP/TEST,
 * RET, y passthrough de control (JMP/JCC/LABEL_DEF/NOP).  Resto: se anyade al
 * migrar el selector (commit 4b/5).  ABI del prologue: funcion HOJA estilo
 * host (la integracion con la VM_ABI -- RBX=ProcessVM*, safepoints -- es el
 * commit 4b).
 */

#ifndef VESTA_JIT_REGALLOC_REWRITE_H
#define VESTA_JIT_REGALLOC_REWRITE_H

#include <cstdint>
#include <string>
#include <vector>

#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/target_reginfo.h"

namespace jit {

    /**
     * @brief Peticion de emision del OSR-entry para @c rewrite_to_physical
     *        (on-stack replacement, Phase D.8).
     *
     * El path por defecto (osr == nullptr en rewrite) conserva el
     * comportamiento actual: si @c VESTA_OSR_COUNT esta activo, instrumenta los
     * back-edges con el contador + la captura del trigger -- esto es el C1, la
     * funcion FUENTE del OSR.
     *
     * Con un @c OsrEmit (mode C2_ENTRY), en cambio, NO se emite el trigger; se
     * APPENDEA un bloque OSR-entry al final de la funcion (el 2o punto de
     * entrada del C2 recompilado) que:
     *   (a) monta el frame normal (mismo prologue que la entry 0),
     *   (b) carga el live-in del loop @c header_block desde
     *       @c proc->osr_buffer[vid] a la ubicacion fisica de cada vid
     *       (R10=base, R11=temp; ninguno es asignable, asi nunca colisionan con
     *       un vid), y
     *   (c) salta al MBlock del header (reanuda el loop a mitad).
     * El offset del bloque se expone via @c osr_entry_label para que el caller
     * lo resuelva post-encode (@c code + label_offsets[osr_entry_label]).
     */
    struct OsrEmit {
        enum Mode { C2_ENTRY };
        Mode     mode         = C2_ENTRY;
        MBlockId header_block = MBLOCK_INVALID; ///< loop header a reanudar
        /**
         * @brief RED DE SEGURIDAD (Phase D.8, paso 4): conjunto de IR VIDs que
         * el C1 capturo al buffer (ordenado).  Si != nullptr, el OSR-entry
         * VERIFICA que cada vid de su live-in (covers(header)) este en este
         * conjunto; si alguno NO esta (el C2 optimizado necesita un valor que el
         * C1 no escribio en el buffer), ABORTA la emision del OSR-entry
         * (@c osr_entry_valid=false) -> el handler no registra la variante ->
         * no hay swap -> el C1 continua su loop (CORRECTO).  Convierte cualquier
         * fallo del invariante "live-in(C2) subset captura(C1)" en una perdida
         * de speedup, NUNCA en corrupcion.  nullptr -> sin verificacion (el
         * recompile plano del 2c, donde C2==C1 trivialmente cumple). */
        const std::vector<uint32_t> *required_captures = nullptr;
        /* --- salida (rellenada por rewrite_to_physical) --- */
        MLabelId osr_entry_label = 0;           ///< label del bloque OSR-entry
        bool     osr_entry_valid = false;       ///< true si se emitio el entry
    };

    /**
     * @brief Reescribe @p vf (forma vreg) a una MFunction fisica.
     *
     * @param vf   Funcion en MachineIR vreg (3-operandos).
     * @param ra   Asignacion del linear-scan (vreg -> reg/slot).
     * @param tri  Descriptor del target (scratch regs, ancho de puntero).
     * @param abi  Convencion del prologue/epilogue (HOST_LEAF o VM).  En VM se
     *             salva/establece RBX = @c ProcessVM* (push rbx + mov rbx,arg0).
     * @param ivs  Intervalos (opcional).  Si != nullptr, el rewrite construye
     *             un @c Stackmap en cada CALL describiendo los slots de los GC
     *             roots vivos a traves (Phase D.7 commit 6).  El resultado va
     *             en @c MFunction::stackmaps y se asocia al CALL via su
     *             @c flags (idx); el encoder rellena el @c pc_offset.
     * @param osr  Peticion de OSR-entry (Phase D.8, opcional).  Si != nullptr,
     *             SUPRIME el trigger C1 y APPENDEA el bloque OSR-entry para el
     *             @c header_block indicado (necesita @c ivs != nullptr para el
     *             live-in).  Rellena @c osr->osr_entry_label / @c osr_entry_valid.
     * @return     Nueva MFunction fisica lista para el encoder.
     */
    MFunction rewrite_to_physical(const MFunction &vf,
                                  const RegAlloc &ra,
                                  const TargetRegInfo &tri,
                                  AbiKind abi = AbiKind::HOST_LEAF,
                                  const IntervalResult *ivs = nullptr,
                                  OsrEmit *osr = nullptr);

    /* ===================================================================== */
    /* OSR runtime glue (Phase D.8, 2c)                                       */
    /* ===================================================================== */

    /**
     * @brief Instala el handler del trigger OSR.  Cuando un loop C1 cruza el
     *        umbral, el codigo emitido invoca el stub interno, que delega en
     *        este handler pasandole el @p loop_id; el handler devuelve la
     *        DIRECCION del OSR-entry del C2 (o 0 si no hay swap).  Lo setea
     *        @c auto_jit con un lookup en su tabla de OSR-entries precompilados.
     */
    void set_osr_handler(uint64_t (*handler)(uint64_t loop_id));

    /**
     * @brief Numero de loops OSR-instrumentados hasta ahora (== loop_ids
     *        asignados).  @c auto_jit lo usa para iterar los loops de una fn
     *        recien compilada y precompilar sus variantes C2-con-OSR-entry.
     */
    uint32_t osr_loop_count();

    /**
     * @brief Info de un loop OSR por @p loop_id.  Rellena @p fn_name_out y
     *        @p header_block_out con el nombre de la funcion y el MBlock del
     *        header.  Devuelve false si el id esta fuera de rango o el loop se
     *        aborto (estado no capturable).
     */
    bool osr_loop_info(uint64_t loop_id, std::string &fn_name_out,
                       uint32_t &header_block_out);

    /**
     * @brief Rellena @p out_vids con los IR VIDs que el C1 capturo al buffer
     *        para @p loop_id (la red de seguridad del C2: el OSR-entry del C2
     *        solo puede leer vids que esten aqui).  Devuelve false si el id esta
     *        fuera de rango o el loop se aborto.
     */
    bool osr_loop_captures(uint64_t loop_id, std::vector<uint32_t> &out_vids);

} // namespace jit

#endif // VESTA_JIT_REGALLOC_REWRITE_H
