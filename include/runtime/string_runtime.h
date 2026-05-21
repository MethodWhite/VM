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

} // namespace runtime

#endif // RUNTIME_STRING_RUNTIME_H
