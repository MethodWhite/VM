# ============================================================================
# vex_lib: frontend del lenguaje Vex (lexer, parser, type checker, lowering,
# facade del compilador).  Diseno aislado segun la decision arquitectonica
# del roadmap: el frontend es una libreria estatica separada que prefigura
# la futura libvesta_rt y permite reutilizarla desde tests, herramientas
# externas o un futuro servicio LSP sin arrastrar todo el binario `vm`.
#
# Dependencias internas:
#  - Ninguna sobre vm/vmcore: el frontend solo conoce ir::IrModule
#    (definido en include/ir/), y consume cabeceras de vex/.  El IR se
#    enlaza desde el ejecutable principal porque sus .cpp se incluyen
#    en el GLOB principal de src/.
#
# Dependencias externas:
#  - Ninguna mas alla de la stdlib de C++17.
# ============================================================================

add_library(vex_lib STATIC
    lexer.cpp
    ast.cpp
    parser.cpp
    type_checker.cpp
    lombok.cpp
    lowering.cpp
    borrow_checker.cpp
    compiler.cpp
    compiler_project.cpp
    mermaid_diagrams.cpp
    graphviz_diagrams.cpp
    html_diagrams.cpp
    comptime_introspect.cpp
    # Sistema de modulos.
    module_resolver.cpp
    module_interop.cpp
    namespace_flatten.cpp
    vexi_format.cpp
    project_cache.cpp
    velb_signature.cpp
    # comptime_vm.cpp vive en src/runtime/ (depende del runtime VM).
    # Transpiler IR -> codigo fuente (C inicial; Java/JS/etc futuro).
    # Vive aqui porque compiler.cpp lo invoca como ultimo paso opcional.
    # Si en el futuro se quiere usar el transpiler standalone, se extrae
    # a su propia libreria port_lib en CMakeLists del root.
    ${CMAKE_SOURCE_DIR}/src/port/transpiler_base.cpp
    ${CMAKE_SOURCE_DIR}/src/port/c/c_backend.cpp
)

# La libreria expone sus headers desde include/vex/; PUBLIC para que los
# consumidores (vm, tests) los hereden automaticamente al linkear.
target_include_directories(vex_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# C++17 minimo (consistente con el resto del proyecto).
target_compile_features(vex_lib PUBLIC cxx_std_17)

# velb_signature.cpp usa OpenSSL EVP_DigestSign / EVP_DigestVerify.
if (WIN32)
    target_include_directories(vex_lib PRIVATE
        ${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64/include)
    target_link_directories(vex_lib PRIVATE
        ${CMAKE_SOURCE_DIR}/libs/Windows/OpenSSL-Win64/lib/VC/x64/MT)
    target_link_libraries(vex_lib PRIVATE libcrypto)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(vex_lib PRIVATE OpenSSL::Crypto)
endif()

# Avisos estrictos en builds nuestros para mantener la regla de "no warnings".
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vex_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()
