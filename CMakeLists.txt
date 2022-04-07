cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico-vdp C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico-vdp
  mode.c
  sys80.c
  test.c
  video.c
)

pico_generate_pio_header(pico-vdp ${CMAKE_CURRENT_LIST_DIR}/sys80.pio)
pico_generate_pio_header(pico-vdp ${CMAKE_CURRENT_LIST_DIR}/video.pio)

pico_enable_stdio_usb(pico-vdp 1)
pico_enable_stdio_uart(pico-vdp 1)
pico_add_extra_outputs(pico-vdp)

target_link_libraries(pico-vdp pico_stdlib hardware_dma hardware_interp hardware_pio)
