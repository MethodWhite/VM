# stdlib/native/collections/CMakeLists.txt
# Plugin nativo de colecciones para VestaVM.
# Proporciona: VestaList, VestaArrayList, VestaHashMap, VestaHashSet.

include(VestaPlugin)

add_vesta_plugin(vesta_collections
    SOURCES    vesta_collections.c
    SDK_DIR    "${VESTA_SDK_DIR}"
    C_STANDARD 11
)
