# stdlib/native/io/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

include(VestaPlugin)

add_vesta_plugin(vesta_io
    SOURCES   vesta_io.c
    SDK_DIR   "${VESTA_SDK_DIR}"
    C_STANDARD 11
)

# A.16: vesta_io usa pthread (mutex global) en POSIX para serializar el
# buffer de salida.  En Windows usa CRITICAL_SECTION (parte de kernel32,
# enlazado automaticamente).
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vesta_io PRIVATE Threads::Threads)
endif()
