cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(photonic_controller C CXX ASM)

pico_sdk_init()

add_executable(photonic_controller
    photonic_controller.c
)

pico_generate_pio_header(photonic_controller
    ${CMAKE_CURRENT_LIST_DIR}/photonic_laser.pio)
pico_generate_pio_header(photonic_controller
    ${CMAKE_CURRENT_LIST_DIR}/photonic_uart.pio)

target_include_directories(photonic_controller PRIVATE
    ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(photonic_controller
    pico_stdlib
    hardware_pio
    hardware_adc
    hardware_uart
    hardware_dma
    hardware_irq
    hardware_clocks)

pico_add_extra_outputs(photonic_controller)
pico_enable_stdio_uart(photonic_controller 0)
