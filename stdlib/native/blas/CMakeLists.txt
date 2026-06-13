# stdlib/native/blas/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

include(VestaPlugin)

add_vesta_plugin(vesta_blas
    SOURCES   vesta_blas.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
    EXTRA_LINK_OPTIONS $<$<NOT:$<BOOL:${WIN32}>>:-lm>
)
