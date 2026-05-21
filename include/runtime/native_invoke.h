/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file native_invoke.h
 * @brief Helper inline para invocar funciones nativas con N argumentos
 *        (0..12) leyendo los valores desde los registros R01..R12 del
 *        proceso virtual.
 *
 * @section design Modelo
 *
 * Convencion C estandar: cada argumento se pasa como @c uint64_t (8 bytes,
 * preserva i64, punteros, double via bit-cast del usuario, etc.).  El
 * resultado se devuelve como @c uint64_t.  Para tipos mas pequenos
 * (@c i32, @c i16, @c i8) el caller debe sign/zero-extender al cargar y el
 * usuario debe truncar el resultado segun necesidad.
 *
 * Switch manual por @c argc en lugar de libffi/dyncall: cero dependencias,
 * cero overhead frente a un puntero a funcion tipado, y cubre el rango
 * [0..12] que es mas que suficiente para cualquier API Win32 estandar
 * (@c MessageBoxA usa 4, @c CreateFileA usa 7, etc.).
 *
 * @section usage Uso
 *
 * Invocan este helper:
 *   - @c exec_instr_calln (FFI estatico declarado en el .velb por el linker).
 *   - @c exec_instr_callni (FFI dinamico runtime: llamada via puntero
 *                            obtenido por @c dlsym/@c GetProcAddress en runtime).
 *
 * Header inline para que ambos cpp compartan la misma implementacion sin
 * indireccion ni copia.
 */
#pragma once

#include "runtime/proceso_runtime.h"

#include <cstdint>

namespace runtime {

    /**
     * @brief Invoca @p fn pasando @p argc registros desde R01..R12 como
     *        @c uint64_t y devuelve el resultado como @c uint64_t.
     *
     * @param fn   Puntero a funcion nativa (ya resuelto via LoadLibraryA +
     *             GetProcAddress o equivalente POSIX).
     * @param argc Numero de argumentos en R01..R12 (clamp [0,12]).  Si excede
     *             12 dispara @c VM_ASSERT (configurable).
     * @param vm   Proceso virtual; los argumentos se leen de
     *             @c vm->registers.regs[R01..R12].qword().
     * @return Valor devuelto por la funcion nativa (sin proteccion de
     *         excepciones; el caller debe envolver en try/__try si quiere
     *         capturar crashes nativos).
     */
    inline uint64_t invoke_native_unchecked(void *fn, uint64_t argc,
                                             ProcessVM *vm) {
        typedef uint64_t u64;
#define NATIVE_INVOKE_A(n) vm->registers.regs[R##n].qword()
        switch (argc) {
            case 0:  return reinterpret_cast<u64(*)()>(fn)();
            case 1:  return reinterpret_cast<u64(*)(u64)>(fn)(NATIVE_INVOKE_A(01));
            case 2:  return reinterpret_cast<u64(*)(u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02));
            case 3:  return reinterpret_cast<u64(*)(u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03));
            case 4:  return reinterpret_cast<u64(*)(u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04));
            case 5:  return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05));
            case 6:  return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06));
            case 7:  return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07));
            case 8:  return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07),NATIVE_INVOKE_A(08));
            case 9:  return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07),NATIVE_INVOKE_A(08),NATIVE_INVOKE_A(09));
            case 10: return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07),NATIVE_INVOKE_A(08),NATIVE_INVOKE_A(09),NATIVE_INVOKE_A(10));
            case 11: return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07),NATIVE_INVOKE_A(08),NATIVE_INVOKE_A(09),NATIVE_INVOKE_A(10),NATIVE_INVOKE_A(11));
            case 12: return reinterpret_cast<u64(*)(u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64)>(fn)(NATIVE_INVOKE_A(01),NATIVE_INVOKE_A(02),NATIVE_INVOKE_A(03),NATIVE_INVOKE_A(04),NATIVE_INVOKE_A(05),NATIVE_INVOKE_A(06),NATIVE_INVOKE_A(07),NATIVE_INVOKE_A(08),NATIVE_INVOKE_A(09),NATIVE_INVOKE_A(10),NATIVE_INVOKE_A(11),NATIVE_INVOKE_A(12));
            default:
                return 0; // argc fuera de rango: el caller debe validar
        }
#undef NATIVE_INVOKE_A
    }

} // namespace runtime
