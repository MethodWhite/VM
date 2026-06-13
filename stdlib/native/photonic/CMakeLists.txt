# stdlib/native/photonic/CMakeLists.txt
# Plugin nativo fotónico para AGI de Resonancia Armónica

cmake_minimum_required(VERSION 3.15)
include(VestaPlugin)
add_vesta_plugin(vesta_photonic
    SOURCES   vesta_photonic.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
target_link_libraries(vesta_photonic PRIVATE m)
