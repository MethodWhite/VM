cmake_minimum_required(VERSION 3.20)
project(VMTests)

# Forzar Debug si no se especifica
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()


message(STATUS "Tests: enlazados contra vmcore/vex_lib/vesta_rt (no recompilan las fuentes)")

# Helper function para crear tests individuales
function(add_single_test PREFIX TEST_FILE ALL_SOURCES)
    
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    set(FINAL_NAME "${PREFIX}_${TEST_NAME}")
    
    # Compilar SOLO el archivo del test y enlazar contra las librerias ya
    # compiladas del proyecto (vmcore = todo salvo main.cpp/vex; vex_lib =
    # frontend; vesta_rt = runtime puro).  Antes cada test recompilaba
    # TODAS las fuentes (212K LOC x 64 tests -> minutos + OOM en paralelo).
    # ALL_SOURCES se ignora salvo que las librerias no esten disponibles.
    if(TARGET vmcore AND TARGET vex_lib)
        add_executable(${FINAL_NAME} ${TEST_FILE})
        # Orden de link: vex_lib (frontend) referencia simbolos de vmcore
        # (ir, comptime, assembly) y viceversa -> grupo de reescaneo para
        # resolver la dependencia circular estatica.
        target_link_libraries(${FINAL_NAME}
            PRIVATE -Wl,--start-group
            PRIVATE vmcore
            PRIVATE vex_lib
            PRIVATE vesta_rt
            PRIVATE -Wl,--end-group
        )
        if(TARGET vpp_lib)
            target_link_libraries(${FINAL_NAME} PRIVATE vpp_lib)
            target_compile_definitions(${FINAL_NAME} PRIVATE VESTA_HAS_PREPROCESSOR=1)
        endif()
    else()
        add_executable(${FINAL_NAME} ${TEST_FILE} ${ALL_SOURCES})
    endif()
    
    # Si estamos en modo AsmIntel, forzar flags en el target
    if (ASM_INTEL)
        target_compile_options(${FINAL_NAME} PRIVATE -S -masm=intel -fverbose-asm -O0)
        
        # No enlazar
        set_target_properties(${FINAL_NAME} PROPERTIES
                EXCLUDE_FROM_ALL TRUE
                LINKER_LANGUAGE CXX
        )
        
        return()  # <-- IMPORTANTE
    endif ()
    
    target_include_directories(${FINAL_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/json
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/cxxopts
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/capstone/include
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/keystone/include
            ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein
    )
    
    # SQLite
    target_link_libraries(${FINAL_NAME}
            PRIVATE sqlite3
            PRIVATE capstone
            PRIVATE keystone
    )
    if (WIN32)
        target_link_libraries(${FINAL_NAME} PRIVATE synchronization)
    endif ()
    
    # OpenSSL
    if (WIN32)
        target_include_directories(${FINAL_NAME} PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_directories(${FINAL_NAME} PRIVATE ${OPENSSL_LIB_DIR})
        target_link_libraries(${FINAL_NAME} PRIVATE libssl libcrypto ws2_32)
    
    else ()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(${FINAL_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif ()
    
    # pthread solo Linux/macOS
    if (NOT WIN32)
        target_link_libraries(${FINAL_NAME} PRIVATE pthread)
    endif ()
    
    if (WIN32)
        target_link_libraries(vm PRIVATE applink)
    endif ()
    
    # Opciones globales para todos los builds
    target_compile_options(${FINAL_NAME} PRIVATE
            -D__STDC_LIMIT_MACROS
            -D__STDC_CONSTANT_MACROS
            -pedantic-errors
    )
    
    # LTO (Link Time Optimization)
    #set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    
    # global
    if (WIN32)
        target_link_options(${FINAL_NAME} PRIVATE
                -static
        )
    endif ()
    
    # release
    target_link_options(${FINAL_NAME} PRIVATE
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
    
    # Enlace estático solo en Windows + Release
    target_link_options(${FINAL_NAME} PRIVATE
            $<$<AND:$<CONFIG:Release>,$<BOOL:${WIN32}>>:
            # --- Enlace estático ---
            -static                    # Binario autónomo
            -static-libgcc             # Libgcc estática
            -static-libstdc++          # Libstdc++ estática
            >
    )
    
    # Opciones por configuracion
    target_compile_options(${FINAL_NAME} PRIVATE
            $<$<CONFIG:Debug>:
            -g -O0 -DVM_DEBUG_CHECKS # añadir checks para depuracion.
            -Wall -Wextra -fno-omit-frame-pointer -DMODE_DEBUG_ENABLE
            #-O2
            -DDEBUG_EMIT        # permitir mostrar el proceso de emision de codigo
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

endfunction()

# Copiar fixtures (test.vel) al directorio de trabajo de los tests, donde
# los tests de lexer/linker/loader los buscan por nombre relativo.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# Generar certificados self-signed para los tests de TLS (net/test2.cpp,
# runtime/test_distrib_session_tls.cpp) que cargan server_cert.pem y
# server_key.pem desde el working dir.  Si openssl no esta disponible los
# tests de TLS fallaran con un mensaje claro en vez de generar un .pem.
find_program(OPENSSL_EXEC openssl)
if(OPENSSL_EXEC)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/server_cert.pem
               ${CMAKE_CURRENT_BINARY_DIR}/server_key.pem
        COMMAND ${OPENSSL_EXEC} req -x509 -newkey rsa:2048
                -keyout ${CMAKE_CURRENT_BINARY_DIR}/server_key.pem
                -out    ${CMAKE_CURRENT_BINARY_DIR}/server_cert.pem
                -days 3650 -nodes -subj "/CN=localhost"
        COMMENT "Generando certificados TLS de test"
        VERBATIM)
    add_custom_target(gen_tls_test_certs ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/server_cert.pem
                ${CMAKE_CURRENT_BINARY_DIR}/server_key.pem)
endif()

# Todos los sources de src/
file(GLOB_RECURSE ALL_PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein_c.c
        ${CMAKE_SOURCE_DIR}/libs/SourceCode/DistanciaLevenshtein/Levenshtein.cpp
)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/runtime/exec_instruction_closure.cpp)

# Fuentes del modulo distribuido (agregadas explicitamente porque GLOB_RECURSE
# puede estar cacheado desde antes de que existiera el directorio src/distrib/)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/distrib/mailbox.cpp)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/distrib/dirty_tracker.cpp)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/distrib/node_registry.cpp)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/distrib/vdp_session.cpp)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/distrib/dist_runtime.cpp)
list(APPEND ALL_PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/runtime/exec_instruction_distrib.cpp)

# Buscar todos los archivos .cpp dentro de tests/ y subcarpetas
file(GLOB_RECURSE TEST_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.cpp)

include(CTest)

foreach (TEST_FILE ${TEST_FILES})
    # Crear un prefijo seguro basado en la ruta relativa
    get_filename_component(TEST_DIR ${TEST_FILE} DIRECTORY)
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    string(REPLACE "/" "_" REL_DIR_SAFE ${TEST_DIR})
    add_single_test("test_${REL_DIR_SAFE}" ${TEST_FILE} "${ALL_PROJECT_SOURCES}")
    add_test(NAME "test_${REL_DIR_SAFE}_${TEST_NAME}"
             COMMAND "test_${REL_DIR_SAFE}_${TEST_NAME}"
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # tests/net/test1.cpp es un DEMO interactivo (abre un server TCP y duerme
    # 30s esperando que el usuario conecte con telnet/nc): no es automatizable
    # para CI.  El binario se sigue compilando para uso manual; ctest lo omite.
    if (TEST_FILE STREQUAL "net/test1.cpp")
        set_tests_properties("test_${REL_DIR_SAFE}_${TEST_NAME}" PROPERTIES DISABLED TRUE)
    endif()
endforeach ()