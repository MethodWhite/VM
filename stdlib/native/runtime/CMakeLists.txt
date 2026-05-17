cmake_minimum_required(VERSION 3.15)
include(VestaPlugin)
add_vesta_plugin(vex_trace
    SOURCES   vex_trace.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
