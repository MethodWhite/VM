# stdlib/native/CMakeLists.txt
# Punto de entrada de la libreria estandar nativa de VestaVM.
#
# Construccion como subproyecto (desde el CMakeLists.txt raiz):
#   cmake -B build .  &&  cmake --build build
#
# Construccion autonoma:
#   cmake -DVESTA_SDK_DIR=/ruta/al/sdk -B build .
#   cmake --build build

cmake_minimum_required(VERSION 3.15)

# Resolver el SDK_DIR si no viene del proyecto padre
if(NOT DEFINED VESTA_SDK_DIR)
    # stdlib/native/ esta dos niveles debajo de la raiz del SDK
    get_filename_component(VESTA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

# Registrar el modulo VestaPlugin para todos los subdirectorios
list(APPEND CMAKE_MODULE_PATH "${VESTA_SDK_DIR}/cmake")

message(STATUS "[stdlib/native] SDK_DIR = ${VESTA_SDK_DIR}")

add_subdirectory(io)
add_subdirectory(math)
add_subdirectory(collections)
add_subdirectory(runtime)
