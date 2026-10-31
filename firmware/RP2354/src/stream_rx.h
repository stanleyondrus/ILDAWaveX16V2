#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STREAM_SAMPLES_PER_PACKET 256

typedef struct {
    uint32_t payload_errors;
    uint32_t received_blocks;
    uint32_t received_samples;
    uint32_t stream_packets;
    uint32_t stream_bad_packets;
    uint32_t stream_dropped_packets;
    uint32_t stream_rx_timeouts;
    uint32_t stream_stale_packets;   /* received across a STOP/CLEAR and discarded */
    uint32_t seq_gaps;               /* breaks in the ESP32's packet sequence within a stream */
    uint32_t seq_missing;            /* packets missing across those breaks */
    uint32_t rx_partial;             /* receives cancelled with part of a packet in */
} stream_rx_status_t;

void stream_rx_init(void);

bool stream_rx_receive_packet(uint32_t timeout_ms);
void stream_rx_process_packet(void);

/* Lowers READY and abandons an armed receive (the SPI link is going unused). */
void stream_rx_cancel(void);

void stream_rx_get_status(stream_rx_status_t *status);