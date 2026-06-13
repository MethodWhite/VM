# stdlib/native/materia/CMakeLists.txt
# Plugin nativo M.A.T.E.R.I.A. - BaseMateria de propósito general

cmake_minimum_required(VERSION 3.15)
include(VestaPlugin)
add_vesta_plugin(vesta_materia
    SOURCES   vesta_materia.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
target_link_libraries(vesta_materia PRIVATE m)
