# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()

set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)

if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_FLASH_SIZE_BYTES 2097152)

include(pico_sdk_import.cmake)

project(ILDAWaveX16V2-RP2350 C CXX ASM)

pico_sdk_init()

# Requires valid implementation in plugins/helios/
option(ENABLE_HELIOS_USB "Enable Helios USB laser DAC protocol (plugin)" OFF)

add_executable(ILDAWaveX16V2-RP2350
    src/main.c
    src/rp_uart.c
    src/rp_settings.c
    src/ring_buffer.c
    src/stream_rx.c
    src/dac80508.c
    src/output_engine.c
)

if(ENABLE_HELIOS_USB)
    target_sources(ILDAWaveX16V2-RP2350 PRIVATE
        plugins/helios/helios_hw.c
        plugins/helios/helios_protocol.c
        plugins/helios/usb_descriptors.c
    )
    target_include_directories(ILDAWaveX16V2-RP2350 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/plugins/helios
    )
    target_compile_definitions(ILDAWaveX16V2-RP2350 PRIVATE ENABLE_HELIOS_USB=1)
endif()

pico_set_program_name(ILDAWaveX16V2-RP2350 "ILDAWaveX16V2-RP2350")
pico_set_program_version(ILDAWaveX16V2-RP2350 "0.2.0") # Keep in sync with "RP_FW_VERSION" in config.h

pico_generate_pio_header(ILDAWaveX16V2-RP2350 ${CMAKE_CURRENT_LIST_DIR}/pio/spi_payload_rx.pio)
pico_generate_pio_header(ILDAWaveX16V2-RP2350 ${CMAKE_CURRENT_LIST_DIR}/pio/dac80508_tx.pio)

target_include_directories(ILDAWaveX16V2-RP2350 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(ILDAWaveX16V2-RP2350
    pico_stdlib
    pico_flash
    pico_multicore
    pico_unique_id
    hardware_gpio
    hardware_uart
    hardware_spi
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_flash
    hardware_watchdog
)

if(ENABLE_HELIOS_USB)
    target_link_libraries(ILDAWaveX16V2-RP2350
        tinyusb_device
        tinyusb_board
    )
endif()

pico_enable_stdio_uart(ILDAWaveX16V2-RP2350 0)
if(ENABLE_HELIOS_USB)
    pico_enable_stdio_usb(ILDAWaveX16V2-RP2350 0)
else()
    pico_enable_stdio_usb(ILDAWaveX16V2-RP2350 1)
endif()

pico_add_extra_outputs(ILDAWaveX16V2-RP2350)
