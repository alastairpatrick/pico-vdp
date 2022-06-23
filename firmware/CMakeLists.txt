cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(firmware C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(firmware
  audio.c
  ay.c
  blit.c
  cdc.c
  hid.c
  keymap.c
  main.c
  perf.c
  scan_out.c
  sys80.c
  video_dma.c
)

target_compile_definitions(firmware PRIVATE
  PICOVDP_OVERCLOCK=1
  PICOVDP_ENABLE_AUDIO=1
  PICOVDP_ENABLE_DUAL_AUDIO=1
)

# Make sure TinyUSB can find tusb_config.h
target_include_directories(firmware PUBLIC ${CMAKE_CURRENT_LIST_DIR})

pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/sys80.pio)
pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/video.pio)

pico_enable_stdio_usb(firmware 0)
pico_enable_stdio_uart(firmware 1)
pico_add_extra_outputs(firmware)

target_link_libraries(firmware
  hardware_dma
  hardware_interp
  hardware_pio
  hardware_pwm
  hardware_sync
  pico_multicore
  pico_stdlib
  tinyusb_board
  tinyusb_host
 )
