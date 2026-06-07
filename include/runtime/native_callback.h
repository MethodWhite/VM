/**
 * @file native_callback.h
 * @brief Callbacks Vex -> C nativos.
 *
 * callback-ABI (2026-06-06).  Permite pasar una funcion Vex como callback
 * a una funcion C nativa (qsort, Win32 wndproc, etc.).  Patron de uso:
 *
 *   extern "msvcrt.dll" {
 *       fn qsort(u8* base, u64 n, u64 size, u64 cmp) -> void;
 *   }
 *
 *   i32 mi_cmp(u8* a, u8* b) {
 *       i32 va = (i32)*(u8*)a;
 *       i32 vb = (i32)*(u8*)b;
 *       return va - vb;
 *   }
 *
 *   i32 main() {
 *       u8[5] arr = {3, 1, 4, 1, 5};
 *       u64 cb = as_native_callback(mi_cmp);   // direccion del codigo callback
 *       qsort(&arr[0], 5, 1, cb);
 *       return 42;
 *   }
 *
 * Implementacion: @c as_native_callback baja a un CALLN al wrapper
 * @c vex_get_native_thunk, que llama a @c jit::compile_native_callback.
 * Este compila la fn Vex DIRECTAMENTE con un entry de ABI C nativo (modo
 * callback del selector): el prologo lee @c ProcessVM* via TLS/call, mueve
 * los args nativos a los slots de los params, y solo salva/restaura el
 * banco de registros VM si el cuerpo puede ensuciarlo (re-entrancia).  Una
 * funcion hoja pura no salva nada.  Reemplaza al thunk hand-emitted previo
 * (que pagaba 16 save + 16 restore por invocacion).
 *
 * Nota: no hay API publica en este header.  @c vex_get_native_thunk es un
 * @c extern "C" en el .cpp, auto-registrado en el virtual_lib_registry.
 */

#ifndef VESTA_RUNTIME_NATIVE_CALLBACK_H
#define VESTA_RUNTIME_NATIVE_CALLBACK_H

#include <cstdint>

#endif // VESTA_RUNTIME_NATIVE_CALLBACK_H
