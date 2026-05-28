/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file abi_checks.cpp
 * @brief Verificacion compile-time de los offsets/sizes declarados
 *        en @c vesta_rt/abi.h contra los layouts reales del runtime.
 *
 * Si cualquier @c static_assert falla, el build se aborta con mensaje
 * claro indicando que la constante del ABI esta desactualizada.  Esto
 * previene drift silencioso entre el header publico (que el JIT/AOT
 * usa) y la implementacion C++ interna del runtime.
 *
 * = Politica =
 *
 * Cuando un layout cambie deliberadamente:
 *
 *   1. Actualizar la struct C++ en @c oop_types.h / @c string_object.h.
 *   2. Bumpear @c VRT_API_VERSION_MAJOR en @c vesta_rt/public.h.
 *   3. Actualizar la constante correspondiente en @c vesta_rt/abi.h.
 *   4. Recompilar JIT y AOT (regenerar todos los .velao caches).
 *
 * Cada uno de estos pasos es OBLIGATORIO; no hacerlo deja un
 * incompatibility latente que puede corromper memoria silenciosamente.
 */

#include "vesta_rt/abi.h"

#include "loader/oop_types.h"
#include "loader/string_object.h"
#include "runtime/proceso_runtime.h"
#include "runtime/vm_registers.h"

#include <cstddef>

/* ========================================================================= */
/* ObjectHeader (24 bytes, alignof=8)                                         */
/* ========================================================================= */

static_assert(sizeof(loader::ObjectHeader) == VESTA_OBJ_HDR_SIZE,
              "ABI drift: sizeof(ObjectHeader) != VESTA_OBJ_HDR_SIZE");

static_assert(offsetof(loader::ObjectHeader, class_ptr)
              == VESTA_OBJ_HDR_CLASS_PTR_OFFSET,
              "ABI drift: offsetof(ObjectHeader, class_ptr)");

static_assert(offsetof(loader::ObjectHeader, flags)
              == VESTA_OBJ_HDR_FLAGS_OFFSET,
              "ABI drift: offsetof(ObjectHeader, flags)");

static_assert(offsetof(loader::ObjectHeader, hash_code)
              == VESTA_OBJ_HDR_HASH_CODE_OFFSET,
              "ABI drift: offsetof(ObjectHeader, hash_code)");

// owner_pid + lock_depth viven empacados en @c monitor_word (8 bytes) en offset 16.
// El layout es: bits 0-47 owner_encoded, bits 48-63 lock_depth.
static_assert(offsetof(loader::ObjectHeader, monitor_word)
              == VESTA_OBJ_HDR_OWNER_PID_OFFSET,
              "ABI drift: offsetof(ObjectHeader, monitor_word)");

static_assert(alignof(loader::ObjectHeader) == 8,
              "ABI drift: alignof(ObjectHeader) != 8");

/* ========================================================================= */
/* StringObject (40 bytes header)                                             */
/* ========================================================================= */

static_assert(sizeof(loader::StringObject) == VESTA_STR_OBJ_SIZE,
              "ABI drift: sizeof(StringObject) != VESTA_STR_OBJ_SIZE");

static_assert(offsetof(loader::StringObject, header)
              == VESTA_STR_HEADER_OFFSET,
              "ABI drift: offsetof(StringObject, header)");

static_assert(offsetof(loader::StringObject, encoding)
              == VESTA_STR_ENCODING_OFFSET,
              "ABI drift: offsetof(StringObject, encoding)");

static_assert(offsetof(loader::StringObject, kind)
              == VESTA_STR_KIND_OFFSET,
              "ABI drift: offsetof(StringObject, kind)");

static_assert(offsetof(loader::StringObject, length)
              == VESTA_STR_LENGTH_OFFSET,
              "ABI drift: offsetof(StringObject, length)");

static_assert(offsetof(loader::StringObject, byte_len)
              == VESTA_STR_BYTE_LEN_OFFSET,
              "ABI drift: offsetof(StringObject, byte_len)");

static_assert(offsetof(loader::StringObject, str_hash)
              == VESTA_STR_HASH_OFFSET,
              "ABI drift: offsetof(StringObject, str_hash)");

/* sizeof(StringObject) ya cubre el offset implicito del @c data[] al
 * final de la struct.  Si el sizeof cambia, los offsets ya fallarian. */

/* ========================================================================= */
/* Conformidad con calling convention                                          */
/* ========================================================================= */

/* El bytecode usa 16 registros generales (R00..R15).  Cualquier
 * cambio rompe el ABI de spawn/CALLN/CALLVIRT. */
static_assert(VESTA_VM_REG_COUNT == 16,
              "ABI drift: el bytecode espera 16 registros GP");

/* ========================================================================= */
/* ProcessVM offsets (JIT acceso via RBX)                               */
/* ========================================================================= */
/*
 * El JIT emite @c mov rbx, rdi (o rcx en Win64) en el prologue y
 * accede al safepoint flag + registros virtuales via offsets fijos.
 * Si el layout de @c ProcessVM cambia, estos asserts forzaran update
 * de las constantes @c VESTA_PROC_*_OFFSET en abi.h.
 *
 * NOTE: @c ProcessVM tiene miembros no-POD (atomicos, vectores,
 * GcHeap).  @c offsetof sobre tipos no-standard-layout es technically
 * UB en el estandar C++ pero GCC/Clang/MSVC lo soportan como
 * extension de forma estable.
 */

static_assert(sizeof(runtime::GeneralRegister) == VESTA_REGISTER_SIZE,
              "ABI drift: sizeof(GeneralRegister) != VESTA_REGISTER_SIZE");

/* Validacion runtime de @c context_registers_vm::regs offset.  Si
 * cambia, ajustar @c VESTA_PROC_REGISTERS_OFFSET en abi.h tras leer
 * el valor real con el mismo @c offsetof. */
static_assert(offsetof(runtime::context_registers_vm, regs)
              == 32,
              "context_registers_vm::regs no esta en offset 32 (ajustar abi.h)");

/* Validacion del flag safepoint en ProcessVM.  Si falla, leer el
 * valor real con @c offsetof y actualizar @c VESTA_PROC_SAFEPOINT_FLAG_OFFSET
 * en @c abi.h, luego bumpear @c VRT_API_VERSION_MAJOR. */
static_assert(offsetof(runtime::ProcessVM, safepoint_flag)
              == VESTA_PROC_SAFEPOINT_FLAG_OFFSET,
              "ABI drift: offsetof(ProcessVM, safepoint_flag) != VESTA_PROC_SAFEPOINT_FLAG_OFFSET");

/* Validacion del array de registros en ProcessVM.
 *
 *   regs_inside_proc = offsetof(ProcessVM, registers) + offsetof(context_registers_vm, regs)
 *
 * Asi VESTA_PROC_REGISTERS_OFFSET cubre directamente acceso al primer
 * register slot.  Cualquier cambio fuerza update. */
static_assert(offsetof(runtime::ProcessVM, registers)
              + offsetof(runtime::context_registers_vm, regs)
              == VESTA_PROC_REGISTERS_OFFSET,
              "ABI drift: regs[0] no esta en VESTA_PROC_REGISTERS_OFFSET");

/* ========================================================================= */
/* ClassInfo + MethodInfo offsets (inline CALLVIRT dispatch)    */
/* ========================================================================= */

static_assert(offsetof(loader::ClassInfo, vtable)
              == VESTA_CLASSINFO_VTABLE_OFFSET,
              "ABI drift: offsetof(ClassInfo, vtable) != VESTA_CLASSINFO_VTABLE_OFFSET");

static_assert(offsetof(loader::MethodInfo, jit_code)
              == VESTA_METHODINFO_JIT_CODE_OFFSET,
              "ABI drift: offsetof(MethodInfo, jit_code) != VESTA_METHODINFO_JIT_CODE_OFFSET");

static_assert(offsetof(loader::MethodInfo, advice_chain)
              == VESTA_METHODINFO_ADVICE_CHAIN_OFFSET,
              "ABI drift: offsetof(MethodInfo, advice_chain) != VESTA_METHODINFO_ADVICE_CHAIN_OFFSET");
