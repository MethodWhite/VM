/**
 * @file native_callback.cpp
 * @brief Resolucion de callbacks Vex -> C nativos.
 *
 * Sprint B.1 (2026-06-01): version inicial via thunk hand-emitted que
 * adaptaba la convencion C nativa -> VM_ABI -> codigo JIT de la fn Vex,
 * salvando/restaurando los 16 registros VM en cada invocacion.
 *
 * callback-ABI (2026-06-06): el thunk se ELIMINA.  En su lugar la fn Vex
 * se compila DIRECTAMENTE con un entry de ABI C nativo (modo callback del
 * selector, ver @c jit::compile_native_callback).  El prologo nativo lee
 * @c ProcessVM* via TLS/call y mueve los args nativos a los slots de los
 * params; solo salva/restaura el banco de registros VM cuando el cuerpo
 * puede ensuciarlo (funciones que llaman / usan raw_asm).  Una funcion
 * hoja pura (e.g. un comparator de qsort) NO salva nada -> ~2x mas rapida
 * que el thunk, que pagaba 16 save + 16 restore por llamada.
 *
 * Patron de uso (sin cambios para el usuario):
 *
 *   extern "msvcrt.dll" { fn qsort(u8* base, u64 n, u64 sz, u64 cmp) -> void; }
 *   i32 mi_cmp(u8* a, u8* b) { return (i32)*(u8*)a - (i32)*(u8*)b; }
 *   i32 main() {
 *       u8[5] arr = {3, 1, 4, 1, 5};
 *       u64 cb = as_native_callback(mi_cmp);   // direccion del codigo callback
 *       qsort(&arr[0], 5, 1, cb);
 *       return 42;
 *   }
 */

#include "runtime/native_callback.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "jit/auto_jit.h"
#include "runtime/exception_runtime.h"
#include "ffi/virtual_lib_registry.h"

/* Wrapper extern "C" para que el builtin Vex `as_native_callback(fn)` lo
 * invoque via CALLN.  Args en proc->registers.regs[R01]=fn_pc, R02=argc.
 * Retorno en proc->registers.regs[R00] = direccion host del codigo callback.
 *
 * argc se ignora: el numero real de params lo deriva el selector del IR de
 * la funcion.  Se mantiene en la firma por compatibilidad con el call site
 * que el lowering ya emite (r1=fn_pc, r2=argc).
 *
 * Registrado en el virtual_lib_registry como
 * @c "vesta_runtime:vex_get_native_thunk". */
extern "C" uint64_t vex_get_native_thunk(uint64_t fn_pc, uint64_t /*argc*/) {
    runtime::ProcessVM *proc = runtime::get_current_executing_process();
    if (proc == nullptr) return 0;
    /* Compila (o reusa de cache) la fn como callback de ABI nativo y
     * devuelve su direccion.  compile_native_callback fuerza el compile
     * aunque el JIT este desactivado por flag (el callback REQUIERE codigo
     * nativo: su direccion se entrega a la API C). */
    return jit::compile_native_callback(proc, fn_pc);
}

/* Auto-registro en el virtual_lib_registry al cargar el binario.  Asi
 * tanto el path --vex (compile) como --run (ejecutar .velb) tienen la
 * fn disponible sin necesidad de llamar al TypeChecker constructor.
 *
 * `__attribute__((used))` evita que `-Wl,--gc-sections` elimine el
 * objeto (no es referenciado desde main).  `__attribute__((constructor))`
 * fuerza ejecucion antes de main, garantizado por GCC/Clang. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, constructor))
static void vex_callback_register_virtual_fn() {
    ffi::register_virtual_fn(
        "vesta_runtime", "vex_get_native_thunk",
        reinterpret_cast<void *>(&::vex_get_native_thunk));
}
#else
namespace {
    struct VexCallbackAutoRegister {
        VexCallbackAutoRegister() {
            ffi::register_virtual_fn(
                "vesta_runtime", "vex_get_native_thunk",
                reinterpret_cast<void *>(&::vex_get_native_thunk));
        }
    };
    static VexCallbackAutoRegister _vex_callback_auto_register;
}
#endif

/* Helper que el main.cpp puede llamar explicitamente para forzar el
 * registro (defense-in-depth si el constructor no se ejecuta por algun
 * motivo de linking). */
extern "C" void runtime_ensure_vex_callback_registered(void) {
    ffi::register_virtual_fn(
        "vesta_runtime", "vex_get_native_thunk",
        reinterpret_cast<void *>(&::vex_get_native_thunk));
}
