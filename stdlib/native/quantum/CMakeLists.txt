# stdlib/native/quantum/CMakeLists.txt
# Plugin nativo de simulacion cuantica para VestaVM

cmake_minimum_required(VERSION 3.15)
include(VestaPlugin)

add_vesta_plugin(vesta_quantum
    SOURCES   vesta_quantum.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
target_link_libraries(vesta_quantum PRIVATE m)
