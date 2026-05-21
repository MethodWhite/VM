cmake_minimum_required(VERSION 3.20)

# Evitar warning CMP0063 de librerias externas
if (POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif ()

#if (WIN32)
#    message(STATUS "Compilando en Windows: activando linkado estático")
#
#    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
#else ()
#    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
#endif ()

set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "Enable verbose build output" FORCE)


# helper para imprimir propiedades relevantes de un target
function(print_target_flags tgt)
    get_target_property(_inc_dirs ${tgt} INCLUDE_DIRECTORIES)
    get_target_property(_compile_opts ${tgt} COMPILE_OPTIONS)
    get_target_property(_compile_defs ${tgt} COMPILE_DEFINITIONS)
    get_target_property(_link_opts ${tgt} LINK_OPTIONS)
    get_target_property(_link_libs ${tgt} LINK_LIBRARIES)

    message(STATUS "---- Target: ${tgt} ----")
    message(STATUS "INCLUDE_DIRECTORIES: ${_inc_dirs}")
    message(STATUS "COMPILE_OPTIONS: ${_compile_opts}")
    message(STATUS "COMPILE_DEFINITIONS: ${_compile_defs}")
    message(STATUS "LINK_OPTIONS: ${_link_opts}")
    message(STATUS "LINK_LIBRARIES: ${_link_libs}")
endfunction()

# no necesitas los ejecutables de fuzzing,
# deshabilitarlos usando opciones de Keystone/LLVM
set(ENABLE_FUZZING OFF CACHE BOOL "" FORCE)


# Configuracion Capstone
set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "Enable ARM")
set(CAPSTONE_AARCH64_SUPPORT ON CACHE BOOL "Enable ARM64")
set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "Enable X86/x64")

# Desactivar todos los demas backends
set(CAPSTONE_MIPS_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_PPC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SPARC_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SYSTEMZ_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_XCORE_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_TMS320C64X_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_M680X_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_EVM_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_MOS65XX_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_WASM_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_BPF_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_RISCV_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_SH_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_TRICORE_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_ALPHA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_HPPA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_LOONGARCH_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_XTENSA_SUPPORT OFF CACHE BOOL "")
set(CAPSTONE_ARC_SUPPORT OFF CACHE BOOL "")


# Configuracion Keystone
set(ENABLE_ARM_TARGET ON CACHE BOOL "")
set(ENABLE_AARCH64_TARGET ON CACHE BOOL "")
set(ENABLE_X86_TARGET ON CACHE BOOL "")

# Desactivar los demas backends
set(ENABLE_MIPS_TARGET OFF CACHE BOOL "")
set(ENABLE_PPC_TARGET OFF CACHE BOOL "")
set(ENABLE_SPARC_TARGET OFF CACHE BOOL "")
set(ENABLE_SYSTEMZ_TARGET OFF CACHE BOOL "")
set(ENABLE_HEXAGON_TARGET OFF CACHE BOOL "")
set(ENABLE_M68K_TARGET OFF CACHE BOOL "")
set(ENABLE_TMS320C64X_TARGET OFF CACHE BOOL "")
set(ENABLE_M680X_TARGET OFF CACHE BOOL "")
set(ENABLE_EVM_TARGET OFF CACHE BOOL "")
set(ENABLE_MOS65XX_TARGET OFF CACHE BOOL "")
set(ENABLE_WASM_TARGET OFF CACHE BOOL "")
set(ENABLE_BPF_TARGET OFF CACHE BOOL "")
set(ENABLE_RISCV_TARGET OFF CACHE BOOL "")
set(ENABLE_SH_TARGET OFF CACHE BOOL "")
set(ENABLE_TRICORE_TARGET OFF CACHE BOOL "")
set(ENABLE_ALPHA_TARGET OFF CACHE BOOL "")
set(ENABLE_HPPA_TARGET OFF CACHE BOOL "")
set(ENABLE_LOONGARCH_TARGET OFF CACHE BOOL "")
set(ENABLE_XTENSA_TARGET OFF CACHE BOOL "")
set(ENABLE_ARC_TARGET OFF CACHE BOOL "")

project(VMProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()


# SQLite library (statica)
add_library(sqlite3 STATIC
        libs/SourceCode/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
        libs/SourceCode/sqlite
)

# Preprocesador vpp (opcional — activado por defecto)
option(VESTA_BUILD_PREPROCESSOR "Compilar el preprocesador vpp e integrarlo en el ensamblador" ON)
if(VESTA_BUILD_PREPROCESSOR)
    # deshabilitar el ejecutable y los tests propios del preprocesador cuando se compila
    # como submodulo del proyecto principal, para no duplicar binarios
    set(VPP_BUILD_EXE   OFF CACHE BOOL "" FORCE)
    set(VPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(preprocessor)
endif()

# Include directories globales
set(VM_INCLUDES
        ${CMAKE_SOURCE_DIR}/include
        libs/SourceCode/json
        libs/SourceCode/cxxopts
        libs/SourceCode/ftxui/include
        libs/SourceCode/capstone/include
        libs/SourceCode/keystone/include
        libs/SourceCode/DistanciaLevenshtein
)
if(VESTA_BUILD_PREPROCESSOR)
    list(APPEND VM_INCLUDES ${CMAKE_SOURCE_DIR}/preprocessor/include)
endif()

# Frontend del lenguaje Vex como libreria estatica aislada (vex_lib).
# Decision arquitectonica: el frontend del HLL se compila
# fuera del GLOB principal y se enlaza al ejecutable como dependencia
# explicita, prefigurando la futura libvesta_rt.
add_subdirectory(src/vex)

# Sources del ejecutable principal.  Se EXCLUYEN los .cpp de src/vex/
# porque ya se compilan dentro de vex_lib (incluirlos en ambos lados
# causaria definicion multiple de simbolos en el linker).
file(GLOB_RECURSE VM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/vex/")
# Port transpiler IR -> codigo (C, futuro Java/JS) vive en vex_lib (ver
# src/vex/CMakeLists.txt).  Excluir para evitar definicion duplicada.
list(FILTER VM_SOURCES EXCLUDE REGEX "/src/port/")
# main.cpp se agrega explicitamente (GLOB_RECURSE con "main.cpp" encontraria
# tambien preprocessor/src/main.cpp y causaria definicion multiple de main)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_closure.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_spimm.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_pattern.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_async.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_weak.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_sync.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_generic.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_distrib.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_exc.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_string.cpp)

# Modulo de programacion distribuida (VDP protocol)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/mailbox.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dirty_tracker.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/node_registry.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/vdp_session.cpp)
list(APPEND VM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dist_runtime.cpp)


set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_USE_LTO OFF CACHE BOOL "" FORCE)

# Capstone (desensamblador)
add_subdirectory(libs/SourceCode/capstone)

# Keystone (desensamblador)
add_subdirectory(libs/SourceCode/keystone)


add_executable(vm ${VM_SOURCES})
target_include_directories(vm
        PRIVATE ${VM_INCLUDES}
)
target_link_libraries(vm
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
        PRIVATE vex_lib
)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(vm PRIVATE vpp_lib)
    target_compile_definitions(vm PRIVATE VESTA_HAS_PREPROCESSOR=1)

    # copiar include_lib junto al ejecutable para que #import <vesta/...> funcione
    # sin necesidad de instalar ni configurar rutas manualmente
    add_custom_command(TARGET vm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/preprocessor/include_lib"
            "$<TARGET_FILE_DIR:vm>/include_lib"
        COMMENT "Copiando stdlib vpp (include_lib) al directorio de salida"
    )
endif()

# OpenSSL
if (WIN32)
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")

    target_include_directories(vm PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vm PRIVATE ${OPENSSL_LIB_DIR})

    target_link_libraries(vm PRIVATE
            libssl
            libcrypto
            ws2_32           # sockets en Windows
            synchronization  # WaitOnAddress / WakeByAddress* (ThreadPool)
    )

    # es necesario en windows
    add_library(applink STATIC
            libs/Windows/OpenSSL-Win64/include/openssl/applink.c
    )

    target_link_libraries(vm PRIVATE applink)
else ()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(vm PRIVATE OpenSSL::SSL OpenSSL::Crypto dl)
endif ()

# Opciones globales para todos los builds
target_compile_options(vm PRIVATE
        -D__STDC_LIMIT_MACROS
        -D__STDC_CONSTANT_MACROS
        -pedantic-errors
)

# LTO (Link Time Optimization)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# global
if (WIN32)
target_link_options(vm PRIVATE
        -static
)
endif()

# release
target_link_options(vm PRIVATE
        $<$<CONFIG:Release>:

        # --- Eliminación de código muerto ---
        -Wl,--gc-sections          # Elimina funciones/datos no usados

        # --- Reducción de tamaño ---
        -Wl,--strip-all            # Elimina todos los símbolos
        -Wl,--as-needed            # Solo enlaza libs realmente usadas
        -Wl,--build-id=none        # No genera build-id (reduce tamaño)

        # --- Flags NO soportados por tu MinGW (comentados) ---
        #-Wl,--icf=all             # Identical Code Folding (solo gold/lld)
        #-Wl,--compress-debug-sections=zlib   # Solo binutils modernos
        #-flto=thin                # mi MinGW no soporta LTO
        >
)

add_library(vesta_install STATIC
        src/install/install.cpp
        src/install/install_options.cpp
        src/install/manifest.cpp
)

if(WIN32)
    target_sources(vesta_install PRIVATE src/install/platform_win.cpp)
    target_link_libraries(vesta_install PRIVATE Shell32 Advapi32 Ole32 User32 Shlwapi)
else()
    target_sources(vesta_install PRIVATE src/install/platform_linux.cpp)
endif()

target_include_directories(vesta_install PUBLIC include)

# Linkar al binario principal:
target_link_libraries(vm PRIVATE vesta_install)

# Enlace estático solo en Windows + Release
target_link_options(vm PRIVATE
        $<$<AND:$<CONFIG:Release>,$<BOOL:${WIN32}>>:
        # --- Enlace estático ---
        -static                    # Binario autónomo
        -static-libgcc             # Libgcc estática
        -static-libstdc++          # Libstdc++ estática
        >
)

# Opciones por configuracion
target_compile_options(vm PRIVATE
        $<$<CONFIG:Debug>:
        -g -O0 -Wall -Wextra -fno-omit-frame-pointer -DMODE_DEBUG_ENABLE
        >
        $<$<CONFIG:Release>:
        -O3
        -march=x86-64
        -mtune=native
        -ffast-math
        -fstrict-aliasing
        -fno-plt
        -fomit-frame-pointer
        -fvisibility=hidden
        -ffunction-sections
        -fdata-sections
        -DNDEBUG
        >
        $<$<CONFIG:RelWithDebInfo>:-fpermissive -g -O1 -fno-omit-frame-pointer>
)


add_library(vmcore STATIC ${VM_SOURCES})
target_include_directories(vmcore PRIVATE ${VM_INCLUDES})
target_link_libraries(vmcore
        PRIVATE sqlite3
        PRIVATE capstone
        PRIVATE keystone
)
if(VESTA_BUILD_PREPROCESSOR)
    target_link_libraries(vmcore PRIVATE vpp_lib)
    target_compile_definitions(vmcore PRIVATE VESTA_HAS_PREPROCESSOR=1)
endif()

if (WIN32)
    target_include_directories(vmcore PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vmcore PRIVATE ${OPENSSL_LIB_DIR})
    target_link_libraries(vmcore PRIVATE libssl libcrypto ws2_32 synchronization applink)
else ()
    target_link_libraries(vmcore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif ()

# ---------------------------------------------------------------------------
# Mejora III: libvesta_rt como libreria independiente.
#
# Subset estricto de @c vmcore que contiene SOLO los componentes del runtime
# puro (GC, scheduler, mailbox, futures, monitores, FFI, class_registry,
# arena, distribucion VDP).  No incluye:
#   - frontend del lenguaje (vex, ir, lexer, parser, emmit)
#   - linker / loader principal del .velb (solo class_registry)
#   - CLI / VSH / REPL
#   - jit, optimizer, profiler, install
#
# Objetivo: el codigo JIT-eado o AOT-compilado en fases futuras 
# podra enlazar contra vesta_rt sin arrastrar el frontend.  Mismo binary
# layout y misma calling convention que las funciones de runtime que el
# bytecode invoca via CALLN.
#
# Estado: MVP -- la libreria compila independientemente y expone los
# simbolos del runtime.  El target @c vm sigue compilando monoliticamente
# todo (sin regresion).  Una integracion mas profunda (vm enlazado contra
# vesta_rt + romper circular loader.h <-> runtime.h) queda para Phase C
# del roadmap, donde se desacople tambien el loader.
# ---------------------------------------------------------------------------
set(VESTA_RT_SOURCES
        # GC - heap generacional + raw allocator
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gc/gc_heap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gc/raw_allocator.cpp

        # Arena manager - VirtualMemory + TLB + ArenaManager
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/VirtualMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/TLB.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/arena/arena_manager.cpp

        # Runtime core - ProcessVM + Scheduler + ManageVM + decoder/exec
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/proceso_runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/manager_runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/decode_instruction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/decode_table.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exception_runtime.cpp

        # Runtime - exec instructions (todas las familias del set)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_alu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_gc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_oop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_closure.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_pattern.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_async.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_spimm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_weak.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_generic.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_distrib.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_exc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_string.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_meta.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_coro.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/exec_instruction_float.cpp

        # Distribucion - VDP protocol + mailbox + dirty tracker
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/mailbox.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dirty_tracker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/node_registry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/vdp_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/distrib/dist_runtime.cpp

        # FFI - puente plugins nativos
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi/native_ffi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi/virtual_lib_registry.cpp

        # Class registry (POO dinamica + reflexion + AOP)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/loader/class_registry.cpp

        # Net (TCP server / TLS) - usado por dist_runtime para VDP
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/net.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net/tcp_server.cpp

        # Util compartido necesario por runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ThreadPool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util/sqlite_singleton.cpp

        # Controller TLS (usado por VDP)
        ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/tls_connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/tls_context.cpp

        # Phase C/D.0 (snapshot 2026-05-13): C ABI estable + ABI checks +
        # infraestructura JIT inicial (code cache, runtime entries, bridge).
        # vesta_rt incluye estos archivos para que el binario JIT-eado
        # pueda enlazarse contra la libreria sin arrastrar el frontend.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vesta_rt/public_wrapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vesta_rt/abi_checks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/code_cache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/runtime_entries.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/x86_encoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/jit_registry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/stack_scan.cpp

        # Opcion W Fase 3b: serializer IR.  Necesario para que el Loader
        # (en vesta_rt) deserialize la seccion @ir del .velb v3.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir/ssa_ir.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ir/ssa_ir_serialize.cpp
)

add_library(vesta_rt STATIC ${VESTA_RT_SOURCES})
target_include_directories(vesta_rt PUBLIC ${VM_INCLUDES})
target_link_libraries(vesta_rt
        PUBLIC sqlite3
)
if (WIN32)
    target_include_directories(vesta_rt PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_directories(vesta_rt PUBLIC ${OPENSSL_LIB_DIR})
    target_link_libraries(vesta_rt PUBLIC libssl libcrypto ws2_32 synchronization applink)
else ()
    target_link_libraries(vesta_rt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif ()
target_compile_options(vesta_rt PRIVATE
        -D__STDC_LIMIT_MACROS
        -D__STDC_CONSTANT_MACROS
        $<$<CONFIG:Release>:-O3 -march=x86-64 -mtune=native -ffast-math
                            -fstrict-aliasing -fno-plt -fomit-frame-pointer
                            -fvisibility=hidden -ffunction-sections
                            -fdata-sections -DNDEBUG>
)

# Anadir subdirectorio de tests
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Configuracion global del SDK de plugins nativos
# Debe quedar fuera de los bloques opcionales para que VESTA_SDK_DIR y
# CMAKE_MODULE_PATH esten disponibles en stdlib, examples y cualquier
# subdirectorio futuro que use add_vesta_plugin().
# ---------------------------------------------------------------------------
set(VESTA_SDK_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Raiz del SDK VestaVM (directorio con include/ffi/vesta_plugin.h)")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Libreria estandar nativa (vesta_io, vesta_math, ...)
option(VESTA_BUILD_STDLIB   "Compilar la libreria estandar nativa de Vesta" ON)
if(VESTA_BUILD_STDLIB)
    add_subdirectory(stdlib/native)
endif()

# Ejemplos y plugins de demostracion
option(VESTA_BUILD_EXAMPLES "Compilar los ejemplos y plugins de ejemplo"    ON)
if(VESTA_BUILD_EXAMPLES)
    add_subdirectory(examples_codes_vm/plugin_ejemplo)
endif()