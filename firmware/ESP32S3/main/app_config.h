#pragma once

#ifndef BOARD_VERSION
#define BOARD_VERSION 2 /* CMake normally supplies this; default keeps editor builds usable. */
#endif

#if BOARD_VERSION == 1
#define PRODUCT_NAME "ILDAWaveX16 V1"
#else
#define PRODUCT_NAME "ILDAWaveX16 V2"
#endif

#define FW_VERSION "0.2.0"

/* Wi-Fi AP */
#if BOARD_VERSION == 1
#define ENABLE_WIFI_AP 1   /* V1 has no Ethernet; AP is the default access method */
#else
#define ENABLE_WIFI_AP 0
#endif
#define WIFI_AP_SSID "ILDAWaveX16-AP"
#define WIFI_AP_PASS "ildawave"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONN 4

/* Wi-Fi STA */
#define ENABLE_WIFI_STA 0
#define WIFI_STA_SSID "<your_ssid>"
#define WIFI_STA_PASS "<your_password>"

/* Network defaults */
#if BOARD_VERSION == 1
#define NET_DEFAULT_HOSTNAME "ildawavex16v1"
#else
#define NET_DEFAULT_HOSTNAME "ildawavex16v2"
#endif
#define NET_DEFAULT_USE_STATIC_IP 0
#define NET_DEFAULT_STATIC_IP "192.168.1.50"
#define NET_DEFAULT_STATIC_NETMASK "255.255.255.0"
#define NET_DEFAULT_STATIC_GATEWAY "192.168.1.1"
#define NET_DEFAULT_STATIC_DNS "1.1.1.1"

/* Discovery and setup portal */
#define ENABLE_MDNS 1
#define MDNS_INSTANCE_NAME NET_DEFAULT_HOSTNAME
#define MDNS_HTTP_SERVICE_NAME "_http"
#define MDNS_HTTP_SERVICE_PROTO "_tcp"
#define MDNS_HTTP_SERVICE_PORT 80

#define ENABLE_CAPTIVE_PORTAL 1
#define CAPTIVE_PORTAL_DNS_PORT 53
#define CAPTIVE_PORTAL_DNS_TTL_SECONDS 60
#define CAPTIVE_PORTAL_DNS_TASK_STACK 6144
#define CAPTIVE_PORTAL_DNS_TASK_PRIORITY 3
#define CAPTIVE_PORTAL_URI "http://" NET_DEFAULT_HOSTNAME ".local/"

/* Network protocol defaults. SD ILDA playback is always enabled. */
#define ENABLE_IDN_PROTOCOL 1
#define ENABLE_ETHERDREAM_PROTOCOL 1
#define ENABLE_ILW_PROTOCOL 1

/* WS2812B status LED */
#define ENABLE_STATUS_LED 1
#define STATUS_LED_GPIO 7
#define STATUS_LED_MAX_BRIGHTNESS 56
#define STATUS_LED_UPDATE_MS 40
#define STATUS_LED_RMT_TIMEOUT_MS 100
#define STATUS_LED_TASK_STACK 4096
#define STATUS_LED_TASK_PRIORITY 2
#define STATUS_LED_RMT_RESOLUTION_HZ 10000000

/* ------------------------------------------------------------------ */
/* V2: ESP32-S3 + W5500 Ethernet + RP2350 DAC                         */
/* ------------------------------------------------------------------ */
#if BOARD_VERSION == 2

/* Shared SPI2: W5500 Ethernet + microSD */
#define SHARED_SPI_HOST SPI2_HOST

#define ETH_SPI_HOST   SHARED_SPI_HOST
#define ETH_CS_GPIO    15
#define ETH_SCLK_GPIO  17
#define ETH_MISO_GPIO  18
#define ETH_MOSI_GPIO  16
#define ETH_RST_GPIO   41
#define ETH_INT_GPIO   42

#define SD_MOUNT_POINT "/sdcard"
#define SD_SPI_HOST    SHARED_SPI_HOST
#define SD_CS_GPIO     14
#define SD_SCLK_GPIO   17
#define SD_MISO_GPIO   18
#define SD_MOSI_GPIO   16
#define SD_DET_GPIO    38

/* RP2350 SPI3 */
#define RP_SPI_HOST             SPI3_HOST
#define RP_CS_GPIO              10
#define RP_SCLK_GPIO            12
#define RP_MISO_GPIO            13
#define RP_MOSI_GPIO            11
#define RP_SPI_PAYLOAD_SPEED_HZ 32000000
/* RP2350 UART constants are in rp_uart.h */

/* ------------------------------------------------------------------ */
/* V1: ESP32-S3 only, direct DAC80508, no Ethernet                    */
/* ------------------------------------------------------------------ */
#else /* BOARD_VERSION == 1 */

/* DAC80508 on SPI3 */
#define DAC_SPI_HOST   SPI3_HOST
#define DAC_CS_GPIO    10
#define DAC_SCLK_GPIO  12
#define DAC_MOSI_GPIO  11
#define DAC_SDO_GPIO   9     /* MISO - not used for write-only DAC */

/* Laser shutter */
#define DAC_SHUTTER_GPIO 8

/* microSD on SPI2/FSPI */
#define SD_MOUNT_POINT "/sdcard"
#define SD_SPI_HOST    SPI2_HOST
#define SD_CS_GPIO     16
#define SD_SCLK_GPIO   14
#define SD_MISO_GPIO   13
#define SD_MOSI_GPIO   15
#define SD_DET_GPIO    38

#define SD_MAX_FILES   32

#endif /* BOARD_VERSION */

#ifndef SD_MAX_FILES
#define SD_MAX_FILES 32
#endif
