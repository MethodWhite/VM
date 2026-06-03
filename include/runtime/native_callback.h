/**
 * @file native_callback.h
 * @brief Generador de thunks C-callable para callbacks Vex -> C nativos.
 *
 * Sprint B.1 (2026-06-01).  Permite pasar una funcion Vex como
 * callback a una funcion C nativa.  Patron de uso:
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
 *       u64 cb = as_native_callback(mi_cmp);   // genera thunk
 *       qsort(&arr[0], 5, 1, cb);
 *       return 42;
 *   }
 *
 * El thunk generado es codigo x86-64 que:
 *   1. Lee args desde RDI/RSI/RDX/RCX (SysV) o RCX/RDX/R8/R9 (Win64).
 *   2. Lee ProcessVM* desde TLS (`t_executing_proc`).
 *   3. Copia los args a proc->registers.regs[R01..RN].
 *   4. Invoca la funcion Vex (interp o JIT-compiled).
 *   5. Lee proc->registers.regs[R00] a RAX (C return value).
 *   6. Restaura regs callee-saved y retorna.
 *
 * Cache global por (vex_fn_pc, argc): si se invoca dos veces
 * `as_native_callback(misma_fn)` se devuelve el mismo thunk.
 */

#ifndef VESTA_RUNTIME_NATIVE_CALLBACK_H
#define VESTA_RUNTIME_NATIVE_CALLBACK_H

#include <cstdint>

namespace runtime {

    /**
     * @brief Obtiene (o genera) un thunk C-callable para una funcion Vex.
     *
     * @param vex_fn_pc PC virtual de la funcion Vex en el .velb (mismo
     *                  valor que el linker resuelve para @c @Absolute("code.fn_name")).
     * @param argc      Numero de argumentos que la fn Vex recibe (max 6
     *                  para ambos ABIs).  El thunk los copia desde los
     *                  regs nativos a los regs VM R01..R<argc>.
     * @return Puntero host a codigo nativo callable con C ABI.  Mismo
     *         resultado en cada invocacion con (vex_fn_pc, argc) iguales.
     *         Retorna 0 si argc > 6 (no soportado v1).
     *
     * Genera el thunk en el code_cache JIT (page-aligned, PAGE_EXECUTE_READ).
     * El thunk vive hasta la destruccion del code_cache (fin del proceso
     * VM).
     */
    uint64_t get_or_generate_native_thunk(uint64_t vex_fn_pc, uint32_t argc);

} // namespace runtime

#endif // VESTA_RUNTIME_NATIVE_CALLBACK_H
