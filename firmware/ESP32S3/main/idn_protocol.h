#pragma once

#include <stdint.h>

#include "esp_err.h"

#define IDN_PROTOCOL_UDP_PORT 7255

/**
 * @brief Start the IDN (ILDA Digital Network) protocol server.
 *
 * Implements:
 *   - IDN-Hello over UDP port 7255 (discovery, ping, service map,
 *     client group mask)
 *   - IDN-RT simplex (0x40) and acknowledged (0x41) realtime streaming,
 *     graceful close (0x44/0x45) and abort (0x46)
 *   - IDN-Stream Laser Projector Continuous Graphic mode (chunk type 0x01)
 *     on channels routed with a valid configuration; no auto-open
 *   - Tag dictionary decoder for the ISP-DB25 compatible default layout
 *     and any producer-supplied layout with X/Y (with optional 16-bit
 *     precision) and R/G/B/I channels
 *   - 1-second link timeout per IDN-Hello spec
 *   - Galvo-hold padding on ring underrun
 *
 * Must be called after net_manager_start() and rp_manager_start().
 */
esp_err_t idn_protocol_start(void);

/**
 * @brief Close any IDN connection and stop its output (local Stop).
 *
 * Served by the IDN server task; waits until the feed task has
 * acknowledged its exit. A sender that keeps streaming opens a new
 * connection and plays again once it re-routes its channel.
 *
 * @return ESP_OK when no IDN output remains, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t idn_protocol_cancel_output(uint32_t timeout_ms);

/*
   Receiver diagnostics, counted since boot unless noted. These are
   application counters, separate from the IDN ACK event flags (which are
   cleared per ACK). Each field is read atomically on its own; the set is
   not one consistent snapshot. "Queued" means placed in the ESP-side IDN
   queue: it is not proof that the samples were rendered.
*/
typedef struct {
    uint32_t rx_datagrams;        /* datagrams of 4+ bytes on the IDN port */
    uint32_t rt_accepted;         /* RT packets passed after sequence checks */
    uint32_t malformed;           /* RT packets failing framing checks; no session effect */
    uint32_t seq_duplicates;      /* dropped: already received */
    uint32_t seq_late;            /* dropped: older and unseen (no reordering) */
    uint32_t seq_stale;           /* dropped: older than the sequence window */
    uint32_t seq_missing;         /* sequence numbers never received */
    uint32_t seq_resyncs;         /* sequence baseline re-established */
    uint32_t ts_before;           /* messages dropped for an earlier timestamp (BPErr) */
    uint32_t ts_resyncs;          /* timestamp timeline re-based after a sender clock restart */
    uint32_t continuity_errors;   /* wave chunks dropped as discontinuous (CTYErr) */
    uint32_t rate_rejects;        /* wave chunks dropped by the rate policy (RGUErr) */
    uint32_t chunk_rejects;       /* wrong chunk types and fragments (CKTErr) */
    uint32_t queued_samples;      /* sender samples placed in the IDN queue */
    uint32_t recovery_samples;    /* blanked samples inserted at discontinuities */
    uint32_t requested_rate_mhz;  /* latest epoch's requested rate in mHz; 0 = none */
    uint32_t selected_rate;       /* rate applied for the current output run; 0 = none */
    uint32_t acks_sent;
} idn_protocol_counters_t;

void idn_protocol_get_counters(idn_protocol_counters_t *out);
