/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file runtime/string_runtime.h
 * @brief API publica para construir @c StringObject directamente desde
 *        C++, reusando exactamente la maquinaria que las instrucciones
 *        de cadena (STRMAKE, STRCAT, etc.) emplean internamente.
 *
 * Util para componentes que necesitan alocar strings en el GcHeap sin
 * pasar por dispatch de bytecode -- por ejemplo el @c ComptimeRuntime
 * de Phase MC, que marshala args @c string del compile-time al VM
 * antes de invocar un @c @Macro lowered.
 *
 * La implementacion vive en @c src/runtime/exec_instruction_string.cpp
 * (mismo TU que las instrucciones de cadena).  Esta cabecera solo
 * expone el subset necesario; el resto de helpers internos siguen
 * @c static a ese TU.
 */

#ifndef RUNTIME_STRING_RUNTIME_H
#define RUNTIME_STRING_RUNTIME_H

#include <cstdint>
#include "gc/gc_heap.h"
#include "loader/string_object.h"   // StringEncoding

namespace runtime {

    class ProcessVM;

    /**
     * @brief Aloca un @c StringObject FLAT en el GcHeap del proceso
     * @c vm con los bytes dados.  Mismo path que la instruccion STRMAKE
     * usa internamente: alocacion non-moving (alloc_pinned), copia de
     * bytes, hash precomputado para strings cortos.
     *
     * @param vm        Proceso virtual propietario del heap.
     * @param data      Buffer de entrada (puede ser @c nullptr para
     *                   string vacio).
     * @param byte_len  Numero de bytes del contenido.
     * @param length    Numero de code-points (para UTF-8/16, puede
     *                   diferir de @c byte_len).  Si pasas
     *                   @c UINT32_MAX, el helper asume ASCII y usa
     *                   @c byte_len como length.
     * @param enc       Codificacion del string.  ASCII por defecto si
     *                   no aplica otra.
     * @return GcHandle del nuevo StringObject, o @c GC_NULL_HANDLE si
     *         la alocacion fallo.
     */
    gc::GcHandle make_string_flat(ProcessVM *vm,
                                   const uint8_t *data,
                                   uint32_t byte_len,
                                   uint32_t length = UINT32_MAX,
                                   loader::StringEncoding enc =
                                       loader::StringEncoding::ASCII) noexcept;

    /**
     * @brief Helper unificado para STRMAKE desde memoria VM.  Misma fast
     * path que @c exec_instr_strmake: stack buf (<=256 B) + single-pass
     * FNV-1a 64-bit + intern lookup-first.  Usado por @c vrt_str_make
     * (JIT) para evitar duplicar logica y bypasear las optimizaciones.
     */
    gc::GcHandle make_string_from_vm_mem(ProcessVM *vm,
                                          uint64_t vm_addr,
                                          uint32_t byte_len) noexcept;

    /**
     * @brief Materializa un StringObject ROPE/SLICE a FLAT (operacion
     * identidad si ya es FLAT).  Usado por STRRAW del JIT antes de
     * devolver el puntero host.
     *
     * @param vm  Proceso virtual.
     * @param h   GcHandle del string a materializar.
     * @return GcHandle del FLAT resultante.  Puede ser distinto de @c h
     *         si el original era ROPE/SLICE.
     */
    gc::GcHandle flatten_string_public(ProcessVM *vm, gc::GcHandle h) noexcept;

    /**
     * @brief Concatena dos StringObjects en un nuevo ROPE O(1).  Si uno
     * de los operandos es vacio, devuelve el otro (sin alocar).  Mismo
     * algoritmo que STRCAT.
     *
     * @param vm  Proceso virtual.
     * @param a   GcHandle del primer string.
     * @param b   GcHandle del segundo string.
     * @return GcHandle del nuevo string concatenado (ROPE) o
     *         @c GC_NULL_HANDLE si alguno de los operandos no es valido.
     */
    gc::GcHandle strcat_public(ProcessVM *vm,
                                gc::GcHandle a,
                                gc::GcHandle b) noexcept;

    /**
     * @brief Compara lexicograficamente dos StringObjects.  Mismo
     * algoritmo que STRCMP.  Materializa ROPE/SLICE si es necesario.
     *
     * @param vm  Proceso virtual.
     * @param a   Primer string.
     * @param b   Segundo string.
     * @return -1, 0 o 1 segun el orden lexicografico.
     */
    int64_t strcmp_public(ProcessVM *vm,
                           gc::GcHandle a,
                           gc::GcHandle b) noexcept;

} // namespace runtime

#endif // RUNTIME_STRING_RUNTIME_H
