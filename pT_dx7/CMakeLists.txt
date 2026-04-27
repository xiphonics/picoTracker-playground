cmake_minimum_required(VERSION 3.13...3.27)

if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board")
endif()

option(USB_MIDI_HOST
       "Build USB MIDI host support instead of the existing USB MIDI device support"
       OFF)
set(TINYUSB_PATH "/home/maks/work/picofire/tinyusb" CACHE PATH
    "Path to an external TinyUSB checkout to use for USB host builds")

# initialize the SDK based on PICO_SDK_PATH before project().
include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(pT_dx7 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (USB_MIDI_HOST AND EXISTS "${TINYUSB_PATH}/src/tusb.h")
    set(PICO_TINYUSB_PATH "${TINYUSB_PATH}" CACHE PATH
        "Path to the TinyUSB checkout used by the Pico SDK" FORCE)
elseif (NOT USB_MIDI_HOST)
    unset(PICO_TINYUSB_PATH CACHE)
endif()

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Require at least Raspberry Pi Pico SDK version 2.0.0")
endif()

if (PICO_TINYUSB_PATH)
    set(ACTIVE_TINYUSB_PATH "${PICO_TINYUSB_PATH}")
else()
    set(ACTIVE_TINYUSB_PATH "${PICO_SDK_PATH}/lib/tinyusb")
endif()

set(TINYUSB_MIDI_HOST_HEADER "${ACTIVE_TINYUSB_PATH}/src/class/midi/midi_host.h")
if (EXISTS "${TINYUSB_MIDI_HOST_HEADER}")
    set(TINYUSB_HAS_MIDI_HOST ON)
else()
    set(TINYUSB_HAS_MIDI_HOST OFF)
endif()

if (USB_MIDI_HOST AND NOT TINYUSB_HAS_MIDI_HOST)
    message(WARNING
        "USB_MIDI_HOST is enabled, but the active TinyUSB checkout at '${ACTIVE_TINYUSB_PATH}' does not provide MIDI host support. "
        "Stage 1 steps 1-4 will build a host-mode transport split, but MIDI host callbacks and raw MIDI receive remain blocked "
        "until TinyUSB with class/midi/midi_host.h is available.")
endif()

add_executable(pT_dx7)
add_subdirectory(third_party/SdFat)
add_subdirectory(src)
add_subdirectory(audio)
add_subdirectory(display)
add_subdirectory(platform)

if (USB_MIDI_HOST)
    if (NOT TARGET tinyusb_host)
        message(FATAL_ERROR "USB_MIDI_HOST requires the tinyusb_host target from the Pico SDK")
    endif()
    set(TINYUSB_TARGET tinyusb_host)
else()
    if (NOT TARGET tinyusb_device)
        message(FATAL_ERROR "USB device MIDI support requires the tinyusb_device target from the Pico SDK")
    endif()
    set(TINYUSB_TARGET tinyusb_device)
endif()

target_link_libraries(pT_dx7 PRIVATE
    pico_stdlib
    hardware_adc
    hardware_clocks
    hardware_dma
    hardware_i2c
    hardware_pio
    hardware_pwm
    hardware_spi
    hardware_uart
    pico_multicore
    hardware_interp
    tinyusb_board
    ${TINYUSB_TARGET}
)

set_property(TARGET pT_dx7 APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--print-memory-usage")

target_compile_definitions(pT_dx7 PUBLIC
  AUDIO_PERF_LOG_ENABLED=1
  PICO_DEFAULT_UART=1
  PICO_DEFAULT_UART_TX_PIN=24
  PICO_DEFAULT_UART_RX_PIN=25
)

if (USB_MIDI_HOST)
  target_compile_definitions(pT_dx7 PUBLIC USB_MIDI_HOST=1)
else()
  target_compile_definitions(pT_dx7 PUBLIC USB_MIDI_HOST=0)
endif()

if (TINYUSB_HAS_MIDI_HOST)
  target_compile_definitions(pT_dx7 PUBLIC TINYUSB_HAS_MIDI_HOST=1)
else()
  target_compile_definitions(pT_dx7 PUBLIC TINYUSB_HAS_MIDI_HOST=0)
endif()

pico_enable_stdio_usb(pT_dx7 0)
pico_enable_stdio_uart(pT_dx7 1)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(pT_dx7)
