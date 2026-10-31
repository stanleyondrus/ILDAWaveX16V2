#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RP_SAMPLE_CHANNELS 8

/*
   Scanner rate limit. Every RATE sent to the RP is clamped to this, whatever
   protocol or page asked for it; the RP firmware (0.2.0+) keeps its own copy
   in flash and clamps as well. 30 kpps is what any ILDA-rated scanner can do
   at the test-pattern angle; raise it for faster scanners.
*/
#define RP_DEFAULT_RATE_LIMIT_HZ 30000u
#define RP_MIN_RATE_LIMIT_HZ 1000u
#define RP_MAX_RATE_LIMIT_HZ 200000u

#define RP_STREAM_SAMPLES_PER_PACKET 256
#define RP_STREAM_HEADER_BYTES 12
#define RP_STREAM_PAYLOAD_BYTES (RP_STREAM_SAMPLES_PER_PACKET * RP_SAMPLE_CHANNELS * 2)
#define RP_STREAM_PACKET_BYTES (RP_STREAM_HEADER_BYTES + RP_STREAM_PAYLOAD_BYTES)

typedef enum {
    RP_TEST_SHAPE_BEAM = 0,
    RP_TEST_SHAPE_SQUARE,
    RP_TEST_SHAPE_CIRCLE,
    RP_TEST_SHAPE_CROSS,
    RP_TEST_SHAPE_HEART,
    RP_TEST_SHAPE_COLOR_SHIFT,   /* white rows with index gaps across R/G/B columns */
} rp_test_shape_t;

typedef struct {
    bool initialized;
    uint32_t ping_count;
    uint32_t error_count;

    esp_err_t last_error;

    bool remote_running;
    uint32_t remote_sample_rate;
    uint32_t remote_payload_errors;

    uint32_t remote_buffer_fill_samples;
    uint32_t remote_buffer_size_samples;
    uint32_t remote_buffer_high_water_samples;
    uint32_t remote_buffer_low_water_samples;
    uint32_t remote_buffer_max_samples;
    bool remote_mirror_x;
    bool remote_mirror_y;
    uint32_t remote_underruns;

    uint32_t rate_limit;              /* ESP32-side limit, saved in NVS */
    uint32_t remote_max_rate;         /* RP's own limit; 0 = RP firmware without one */
    uint32_t remote_requested_rate;   /* last rate asked of the RP before clamping */
    uint32_t remote_max_lit_step;     /* largest lit X/Y step since START, 0..65535 */
    uint32_t remote_lit_big_steps;    /* lit steps >= 1/8 full scale since START */
    uint32_t remote_blank_big_steps;
    int32_t  remote_color_shift_us;      /* RP 0.2.0+: colour delay against X/Y */
    uint32_t remote_color_shift_samples;
    bool     remote_color_shift_saved;   /* false: applied while running, not in RP flash */
    uint32_t remote_stale_packets;

    uint32_t remote_stream_packets;
    uint32_t remote_stream_bad_packets;
    uint32_t remote_stream_dropped_packets;
    uint32_t remote_stream_rx_timeouts;
    uint32_t remote_seq_gaps;         /* RP: breaks in the packet sequence within a stream */
    uint32_t remote_seq_missing;      /* RP: packets missing across those breaks */
    uint32_t remote_rx_partial;       /* RP: receives that timed out mid-packet */

    /* Packets the ESP32 failed to deliver: READY never rose (packet dropped),
       the SPI transfer failed, or READY never fell after it. */
    uint32_t payload_ready_timeouts;
    uint32_t payload_spi_errors;
    uint32_t payload_ack_timeouts;

    bool test_stream_running;
    uint32_t test_stream_blocks_sent;
    char remote_fw_version[64];
} rp_status_t;

esp_err_t rp_manager_start(void);
esp_err_t rp_manager_ping(void);
esp_err_t rp_manager_get_remote_status(void);
esp_err_t rp_manager_output_start(void);
esp_err_t rp_manager_output_stop(void);
esp_err_t rp_manager_output_force_stop(void);
esp_err_t rp_manager_output_discard_unstarted(void);
esp_err_t rp_manager_clear_buffer(void);
esp_err_t rp_manager_set_rate(uint32_t rate_hz);
/* As rp_manager_set_rate; *applied receives the rate the RP now runs. */
esp_err_t rp_manager_set_rate_applied(uint32_t rate_hz, uint32_t *applied);
esp_err_t rp_manager_set_rate_limit(uint32_t limit_hz);
/* Applies at once; the RP saves it only while output is stopped. */
esp_err_t rp_manager_set_color_shift(int32_t shift_us);
/* Saves the applied colour shift to RP flash; fails while output runs. */
esp_err_t rp_manager_save_color_shift(void);
uint32_t rp_manager_get_rate_limit(void);
/* The rate the RP runs now: kept current by RATE replies, limit changes and STATUS polls. */
uint32_t rp_manager_get_sample_rate(void);
esp_err_t rp_manager_set_buffer_config(uint32_t size_samples,
                                       uint32_t high_water_samples,
                                       uint32_t low_water_samples);
esp_err_t rp_manager_set_mirror(bool mirror_x, bool mirror_y);
void rp_manager_set_realtime_streaming(bool enabled);
esp_err_t rp_manager_send_test_block(void);
esp_err_t rp_manager_start_test_stream(void);
esp_err_t rp_manager_start_test_shape(rp_test_shape_t shape);
void rp_manager_set_test_color_mask(uint8_t mask);
esp_err_t rp_manager_stop_test_stream(void);
esp_err_t rp_manager_send_samples_packet(const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS]);
esp_err_t rp_manager_wait_payload_idle(uint32_t timeout_ms);
void rp_manager_get_status(rp_status_t *status);
