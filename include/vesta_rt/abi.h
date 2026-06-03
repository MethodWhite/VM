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
 * @file vesta_rt/abi.h
 * @brief Constantes ABI fijas para que codigo generado (JIT/AOT) pueda
 *        acceder a estructuras runtime sin incluir cabeceras C++.
 *
 * = Diseno =
 *
 * Estas constantes son los offsets EN BYTES de cada campo dentro de su
 * struct.  Codigo JIT-eado emite instrucciones como:
 *
 *   mov rax, [rdi + VESTA_OBJ_HDR_CLASS_PTR_OFFSET]   ; rax = obj->class_ptr
 *
 * Sin esto, el JIT tendria que hardcodear offsets que rotarian cada
 * vez que cambia el layout interno.  Con este header, cualquier cambio
 * de layout requiere recompilar JIT + AOT pero NO afecta a binarios
 * AOT ya producidos (estos llevan el offset incrustado).
 *
 * = Verificacion =
 *
 * El runtime (al inicio de @c vesta_rt::init_abi_checks) ejecuta
 * @c static_assert (en C++) y/o @c assert (en runtime) que confirman:
 *
 *   - sizeof(ObjectHeader)       == VESTA_OBJ_HDR_SIZE
 *   - offsetof(ObjectHeader, X)  == VESTA_OBJ_HDR_X_OFFSET
 *   - sizeof(StringObject)       == VESTA_STR_OBJ_SIZE
 *   ... etc
 *
 * Si alguno falla, el build aborta inmediatamente (compile-time) o
 * el runtime hace abort() al primer chequeo.  No se permite drift.
 *
 * = Actualizaciones =
 *
 * Bumpear @c VRT_API_VERSION_MAJOR en @c public.h cuando se anyada,
 * elimine o reordene un campo de un struct cubierto aqui.  Cambios de
 * tipo (uint32_t -> uint64_t) tambien requieren bump major.
 */

#ifndef VESTA_RT_ABI_H
#define VESTA_RT_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* ObjectHeader (24 bytes, alignof=8)                                         */
/* ========================================================================= */
/*  +0  [8]  class_ptr   ClassInfo*                                           */
/*  +8  [4]  flags       OBJ_FLAG_* bitmask                                   */
/*  +12 [4]  hash_code   identidad lazy                                       */
/*  +16 [4]  owner_pid   local_pid del propietario del monitor (0 = libre)    */
/*  +20 [2]  lock_depth  contador reentrante                                  */
/*  +22 [2]  _mon_pad    padding                                              */
/* ========================================================================= */

#define VESTA_OBJ_HDR_SIZE                24

#define VESTA_OBJ_HDR_CLASS_PTR_OFFSET    0
#define VESTA_OBJ_HDR_FLAGS_OFFSET        8
#define VESTA_OBJ_HDR_HASH_CODE_OFFSET    12
#define VESTA_OBJ_HDR_OWNER_PID_OFFSET    16
#define VESTA_OBJ_HDR_LOCK_DEPTH_OFFSET   20
/* offset 22-23: _mon_pad (no usado por codigo generado) */

/* Bits del campo flags (OBJ_FLAG_*) */
#define VESTA_OBJ_FLAG_GC_OWNED           (1u << 0)
#define VESTA_OBJ_FLAG_INTERNED           (1u << 1)
#define VESTA_OBJ_FLAG_PINNED             (1u << 2)

/* ========================================================================= */
/* StringObject (40 bytes header + data[] inline)                             */
/* ========================================================================= */
/*  +0  [24] header      ObjectHeader                                         */
/*  +24 [1]  encoding    StringEncoding (0=ASCII 1=ANSI 2=UTF8 3=UTF16 4=UTF32)*/
/*  +25 [1]  kind        StringKind bits[1:0] (0=FLAT 1=ROPE 2=SLICE)         */
/*                       + bit[7] = is_interned                               */
/*  +26 [2]  _pad        padding                                              */
/*  +28 [4]  length      code-point count                                     */
/*  +32 [4]  byte_len    byte count del contenido logico                      */
/*  +36 [4]  str_hash    FNV-1a cacheado (0 = no calculado)                   */
/*  +40        data[]    contenido (variable, segun kind)                     */
/* ========================================================================= */

#define VESTA_STR_OBJ_SIZE                40

#define VESTA_STR_HEADER_OFFSET           0
#define VESTA_STR_ENCODING_OFFSET         24
#define VESTA_STR_KIND_OFFSET             25
#define VESTA_STR_LENGTH_OFFSET           28
#define VESTA_STR_BYTE_LEN_OFFSET         32
#define VESTA_STR_HASH_OFFSET             36
#define VESTA_STR_DATA_OFFSET             40

/* Valores de encoding */
#define VESTA_STR_ENC_ASCII               0
#define VESTA_STR_ENC_ANSI                1
#define VESTA_STR_ENC_UTF8                2
#define VESTA_STR_ENC_UTF16               3
#define VESTA_STR_ENC_UTF32               4

/* Valores de kind */
#define VESTA_STR_KIND_FLAT               0
#define VESTA_STR_KIND_ROPE               1
#define VESTA_STR_KIND_SLICE              2
#define VESTA_STR_KIND_INTERNED_BIT       0x80

/* ========================================================================= */
/* ClassInfo / MethodInfo / FrameHeader                                       */
/* ========================================================================= */
/*
 * NOTE v1: estos structs contienen miembros C++ no-POD (std::string,
 * std::vector via stringx).  Los offsets dependen de la implementacion
 * de la libstdc++ del compilador y no son estables cross-build sin
 * verificacion al inicio.
 *
 * Para Phase D.0 NO exponemos sus offsets aqui.  El JIT en Phase D.1
 * los obtendra via un descriptor inicializado en startup
 * (@c vrt_class_layout / @c vrt_method_layout) que el runtime calcula
 * con @c offsetof y publica como const globals.  Asi codigo AOT
 * compilado por un toolchain se enlaza correctamente aunque el
 * runtime se recompile con otro libstdc++.
 *
 * Hoy son STUBS reservados; se rellenaran en D.1.b cuando el
 * instruction selector emita la primera @c callvirt JIT.
 */

/* ========================================================================= */
/* ProcessVM (offsets criticos para JIT-eado)                                 */
/* ========================================================================= */
/*
 * El JIT-eado mantiene @c ProcessVM* en RBX durante toda la vida de
 * la funcion (cargado del primer arg en el prologue).  Para emitir
 * polls de safepoint y para leer/escribir los registros virtuales
 * del bytecode, necesita offsets fijos:
 *
 *   safepoint_flag: byte que el GC setea cuando quiere pausar el
 *                   proceso para stack scan.  Poll desde JIT:
 *
 *                       cmp byte [rbx + SAFEPOINT_FLAG_OFFSET], 0
 *                       jne handler_lbl
 *
 *                   El handler invoca @c vrt_safepoint_handler.
 *
 *   registers_offset: array de 16 registros VM (R0..R15) de 8 bytes
 *                     cada uno (estructura Register con un union).
 *                     Para acceder R[N] desde JIT:
 *
 *                       mov rax, [rbx + REGISTERS_OFFSET + N*8]
 *
 * Los valores AQUI son provisionales; @c abi_checks.cpp valida que
 * coincidan con @c offsetof(runtime::ProcessVM, X) en compile-time.
 * Si la struct cambia, el build aborta con mensaje claro.
 */

/// Offset del byte safepoint_flag dentro de ProcessVM.  Verificado
/// en compile-time por @c abi_checks.cpp.  PRIMER campo de ProcessVM
/// a proposito: permite que el poll JIT-eado sea
/// @c cmp byte [rbx], 0 (4 bytes, sin displacement), mas compacto
/// que disp8 (5 bytes) o disp32 (8 bytes).
#define VESTA_PROC_SAFEPOINT_FLAG_OFFSET   0

/// Offset del array @c registers.regs[0..15] dentro de ProcessVM.
/// Verificado en compile-time.  Cada registro ocupa 8 bytes.
/// Para R0..R3 (offset 96..120), el encoder puede usar disp8 (6 bytes).
/// Para R4..R15 (offset 128..216), se usa disp32 (9 bytes por acceso).
#define VESTA_PROC_REGISTERS_OFFSET        96

/// Size en bytes de cada Register (slot de 8 bytes con union).
#define VESTA_REGISTER_SIZE                8

/// Numero total de registros VM (R0..R15).
#define VESTA_PROC_REGISTER_COUNT          16

/// Offset de @c proc->registers.stack_pointer dentro de ProcessVM.
/// El stack_pointer es el primer campo de @c context_registers_vm,
/// y @c registers vive en offset (VESTA_PROC_REGISTERS_OFFSET - 32)
/// dentro de ProcessVM.  Por tanto stack_pointer = registers_off + 0.
/// Verificado en compile-time por @c abi_checks.cpp.
///
/// Usado por Phase D.jit-mem-model VM-STACK: el JIT modifica el VM-RSP
/// al ejecutar ALLOCAs vex (consistente con interp `subsp`), salvando
/// y restaurando el valor en prologue/epilogue.
#define VESTA_PROC_STACK_POINTER_OFFSET    64

/// Offset de @c proc->registers.base_pointer (similar al anterior).
#define VESTA_PROC_BASE_POINTER_OFFSET     72

/// El offset de @c exc_frame_stack dentro de @c ProcessVM NO es estable
/// cross-build porque la struct tiene miembros no-POD antes (atomicos,
/// vectores, GcHeap).  En lugar de un #define, se computa en runtime
/// con @c offsetof y se pasa al selector via @c SelectorOptions::exc_frame_stack_offset.
/// El JIT lo embebe como disp32 en las instrucciones inline.  Mismo patron
/// que @c nursery_bump_offset.

#ifdef __cplusplus
namespace vesta_rt {
    /* Phase D.jit-mem-model INLINE-CACHE: offsets resueltos en runtime
     * via offsetof.  Defined en abi_checks.cpp. */
    extern const int32_t kProcVmMemOffset;
    extern const int32_t kVmMemCachedPageVaddrOffset;
    extern const int32_t kVmMemCachedPageHostOffset;
}
#endif

/* ----------------------------------------------------------------------- */
/* VirtualMemory page cache offsets (Phase D.jit-mem-model INLINE-CACHE)  */
/* ----------------------------------------------------------------------- */
/* El JIT emite inline el page cache hit para LOAD/STORE sobre VM-addrs:
 *   page = vaddr & ~0xFFF
 *   if (page == proc->vm_mem.cached_page_vaddr
 *    && (vaddr & 0xFFF) + size <= 4096)
 *       host_ptr = proc->vm_mem.cached_page_host + (vaddr & 0xFFF)
 *       <native mov>
 *   else
 *       call vrt_vm_read_u<size> / write_u<size>
 *
 * Coste hit (95% accesos secuenciales en mismo stack page): ~5 ns
 * Coste miss/cross-page: ~30 ns (call al runtime entry).
 *
 * Los offsets NO se pueden #define: dependen del compilador y el layout
 * runtime de ProcessVM/VirtualMemory.  Se exponen via variables extern
 * resueltas en runtime por @c abi_checks.cpp y se pasan al selector
 * via @c SelectorOptions::vm_mem_cached_page_*_offset. */

/* ----------------------------------------------------------------------- */
/* ClassInfo + MethodInfo offsets (inline dispatch CALLVIRT)  */
/* ----------------------------------------------------------------------- */
/* Layout (oop_types.h):
 *   ClassInfo {
 *     stringx    name;            // 0..15   (uint8_t* + uint32_t + 4 pad)
 *     uint64_t   flags;           // 16..23
 *     uint32_t   instance_size;   // 24..27
 *     uint32_t   _pad;            // 28..31
 *     ClassInfo**supers;          // 32..39
 *     size_t     super_count;     // 40..47
 *     ClassInfo**interfaces;      // 48..55
 *     size_t     interface_count; // 56..63
 *     FieldInfo* fields;          // 64..71
 *     size_t     field_count;     // 72..79
 *     MethodInfo**vtable;         // 80..87   <- VTABLE OFFSET
 *     size_t     vtable_size;     // 88..95
 *   }
 *
 *   MethodInfo {
 *     stringx           name;          // 0..15
 *     stringx           descriptor;    // 16..31
 *     uint64_t          flags;         // 32..39
 *     ClassInfo        *owner_class;   // 40..47
 *     FieldInfo        *args;          // 48..55
 *     size_t            arg_count;     // 56..63
 *     FieldInfo        *return_type;   // 64..71
 *     uint64_t          code_vaddr;    // 72..79
 *     size_t            code_size;     // 80..87
 *     HandlerException *handlers;      // 88..95
 *     size_t            handler_count; // 96..103
 *     void             *jit_code;      // 104..111  <- JIT_CODE OFFSET
 *   }
 *
 * Estos offsets se validan con static_assert en abi_checks.cpp.  Si cambia
 * el layout en oop_types.h, el build falla aqui forzando actualizacion.
 */
#define VESTA_CLASSINFO_VTABLE_OFFSET      80
/* Offset del puntero static_data (uint8_t*) en ClassInfo.  Apunta a un
 * bloque host de bytes con los valores de los static fields.  Usado por
 * GETSTATIC / SETSTATIC en JIT para load/store directo sin runtime call:
 *
 *   mov rax, [rax + VESTA_CLASSINFO_VTABLE_OFFSET + 16] ; (vtable=80, vtable_size=8, padding+static_data=16+x)
 *
 * El offset exacto se valida via static_assert en abi_checks.cpp.
 */
#define VESTA_CLASSINFO_STATIC_DATA_OFFSET 96
#define VESTA_METHODINFO_JIT_CODE_OFFSET   104
/* Offset del puntero advice_chain (cadena AOP BEFORE/AFTER/AROUND).
 * Si != NULL, el inline dispatch JIT debe caer al slow path para que
 * la cadena se ejecute correctamente.  Sin esto, el JIT salta al body
 * del metodo target ignorando todos los advices. */
#define VESTA_METHODINFO_ADVICE_CHAIN_OFFSET 152

/* ========================================================================= */
/* ExceptionFrame + ProcessVM::exc_frame_stack (inline tryleave en JIT)       */
/* ========================================================================= */
/*
 * Layout de ProcessVM::ExceptionFrame (struct anidada en proceso_runtime.h):
 *   +0  [8]   handler_pc         direccion absoluta del catch
 *   +8  [8]   type               ClassInfo* (NULL = catch-all)
 *   +16 [8]   saved_rsp
 *   +24 [8]   saved_rbp
 *   +32 [8]   saved_frame_stack
 *   +40 [128] saved_regs[16]     snapshot R0..R15
 *   +168 [8]  prev               siguiente frame (linked list)
 *   total: 176 bytes
 *
 * El JIT inlinea tryleave como:
 *
 *   mov rax, [rbx + EXC_FRAME_STACK_OFFSET]   ; top
 *   test rax, rax
 *   je  no_op
 *   mov rcx, [rax + EXC_FRAME_PREV_OFFSET]    ; prev
 *   mov [rbx + EXC_FRAME_STACK_OFFSET], rcx   ; head = prev
 *  no_op:
 *
 * 5 instrucciones x86-64 (~5-8 ciclos).  El frame popped queda leaked en
 * heap raw (~176 bytes/try); el coste por programa con N tries es N*176B.
 * Aceptable para v1; un free-list per-proc podria eliminar el leak.
 *
 * Verificado en abi_checks.cpp con static_assert.
 */
#define VESTA_EXC_FRAME_PREV_OFFSET        168
#define VESTA_EXC_FRAME_SIZE               176

/* ========================================================================= */
/* Codigos de FatalError (usados por vrt_throw_fatal)                         */
/*                                                                            */
/* CRITICO: estos valores DEBEN coincidir con el enum FatalKind definido en   */
/* @c include/runtime/exception_runtime.h.  El throw_fatal del runtime usa    */
/* el enum interno; si los valores no coinciden, el kind pasado por el JIT    */
/* se traduce a un kind distinto al imprimir el error.  Fix 2026-05-16:       */
/* sincronizados con FatalKind.                                               */
/* ========================================================================= */

#define VESTA_FATAL_NULL_POINTER            1
#define VESTA_FATAL_DIVISION_BY_ZERO        2
#define VESTA_FATAL_STACK_OVERFLOW          3
#define VESTA_FATAL_STACK_UNDERFLOW         4
#define VESTA_FATAL_ILLEGAL_INSTRUCTION     5
#define VESTA_FATAL_INVALID_SYSCALL         6
#define VESTA_FATAL_SEGMENTATION_FAULT      7
#define VESTA_FATAL_NATIVE_CRASH            8
#define VESTA_FATAL_NATIVE_EXCEPTION        9
#define VESTA_FATAL_OUT_OF_MEMORY           10
#define VESTA_FATAL_USER_ABORT              11
/* Codigos NO presentes en FatalKind (specific to ABI public): */
#define VESTA_FATAL_INDEX_OUT_OF_BOUNDS     12
#define VESTA_FATAL_TYPE_MISMATCH           13

/* ========================================================================= */
/* Convenciones de calling                                                    */
/* ========================================================================= */

/*
 * El JIT-eado de VestaVM usa una convencion FIJA, independiente del
 * ABI del host (System V vs Win64):
 *
 *   Entry de funcion JIT:
 *     - rdi (SysV) / rcx (Win64): ProcessVM *proc
 *     - Los argumentos del bytecode estan en proc->registers.regs[1..N]
 *     - El returncode debe escribirse a proc->registers.regs[0]
 *     - argc esta en proc->registers.regs[15]
 *
 *   Llamada a un runtime entry (vrt_*):
 *     - Convencion C nativa del host (SysV o Win64).
 *     - El trampoline @c vrt_proc* va en rdi/rcx, demas args siguen
 *       las reglas del compilador C.
 *
 *   Llamada de codigo JIT a otra funcion JIT:
 *     - Convencion VM (ProcessVM* en rdi/rcx, args via proc->registers).
 *     - Sin traduccion de regs, cero overhead.
 *
 *   Llamada de codigo JIT al interprete:
 *     - Via trampoline @c return_from_jit en @c interp_jit_bridge.
 */

#define VESTA_VM_REG_COUNT                  16

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_RT_ABI_H */
