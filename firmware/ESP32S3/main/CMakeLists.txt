# Set BOARD_VERSION=1 for ILDAWaveX16 V1
# Set BOARD_VERSION=2 for ILDAWaveX16 V2
if(NOT DEFINED BOARD_VERSION)
    set(BOARD_VERSION 2)
endif()

message(STATUS "Building for BOARD_VERSION=${BOARD_VERSION}")

set(COMMON_SRCS
    "main.c"
    "net_manager.c"
    "status_led.c"
    "web_server.c"
    "sd_manager.c"
    "ilda_player.c"
    "ilda_preview.c"
    "protocol_stream_stats.c"
    "protocol_settings.c"
    "etherdream_protocol.c"
    "idn_protocol.c"
    "idn_decoder.c"
    "ilw_protocol.c"
)

if(BOARD_VERSION EQUAL 1)
    list(APPEND COMMON_SRCS "rp_manager_v1.c")
else()
    list(APPEND COMMON_SRCS "rp_manager.c" "rp_uart.c")
endif()

set(COMMON_REQUIRES
    esp_wifi
    esp_event
    esp_netif
    esp_eth
    esp_http_server
    fatfs
    sdmmc
    nvs_flash
    esp_timer
    esp_driver_uart
    esp_driver_gpio
    esp_driver_spi
    esp_driver_rmt
    lwip
    espressif__mdns
)

if(BOARD_VERSION EQUAL 2)
    list(APPEND COMMON_REQUIRES espressif__w5500)
endif()

idf_component_register(
    SRCS ${COMMON_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${COMMON_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_VERSION=${BOARD_VERSION})
