#pragma once

/*
 * idn_timing.h
 *
 * Pure-C timing and diagnostic helpers for the IDN receiver: RT sequence
 * tracking, message timestamp chronology, per-channel wave continuity,
 * sample-rate policy, latched ACK events, and ACK/scan field serialization.
 *
 * No ESP-IDF, FreeRTOS or socket dependency: everything here is a
 * deterministic function of its arguments so it can be tested on a host.
 * Nothing here is thread-safe; callers that share a structure between tasks
 * must wrap each call in their own critical section.
 *
 * References: IDN-Hello draft 2022-03-27 (sections 4.1.2, 6, 6.3.3) and
 * IDN-Stream revision 002 (sections 2.1, 6.1). Policies marked "project
 * policy" are this firmware's choices, not requirements of the standards.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -- ACK result codes (Hello 6.3.3) ----------------------------------------- */
#define IDN_ACK_RESULT_OK               0x00u   /* received and passed to the session */
#define IDN_ACK_RESULT_EMPTY_CLOSE      0xEBu   /* empty close without a connection */
#define IDN_ACK_RESULT_OCCUPIED         0xECu   /* no session available */
#define IDN_ACK_RESULT_EXCLUDED         0xEDu   /* client group excluded */
#define IDN_ACK_RESULT_INVALID_PAYLOAD  0xEEu
#define IDN_ACK_RESULT_OTHER_ERROR      0xEFu

/* -- Input event flags: link/session scope (Hello 6.3.3) -------------------- */
#define IDN_INEV_IRAERR         0x8000u     /* internal resource or assertion error */
#define IDN_INEV_CCERR          0x2000u     /* non-void message on closed channel */
#define IDN_INEV_BPERR          0x0800u     /* timestamp before already processed ones; dropped */
#define IDN_INEV_LAERR          0x0400u     /* late arrival */
#define IDN_INEV_OVERR          0x0200u     /* overrun: more arrived than can be buffered */
#define IDN_INEV_MVERR          0x0100u     /* early message validation error */
#define IDN_INEV_SEQ_NOT_NEXT   0x0010u     /* SeqErr type 1: not strictly +1 */
#define IDN_INEV_SEQ_DUPLICATE  0x0020u     /* SeqErr type 2: duplicate */
#define IDN_INEV_SEQ_MISSING    0x0040u     /* SeqErr type 3: missing */
#define IDN_INEV_ORDER          0x0008u     /* resolved (reordered) timestamp discontinuity */
#define IDN_INEV_NEW            0x0001u     /* new connection since last report */

/* -- Pipeline event flags: channel scope (Hello 6.3.3) ---------------------- */
#define IDN_PLEV_IAPERR         0x8000u     /* internal assertion or processing error */
#define IDN_PLEV_DVIERR         0x4000u     /* device irregularity */
#define IDN_PLEV_PVLERR         0x2000u     /* payload validation/decoding */
#define IDN_PLEV_RGUERR         0x1000u     /* range check / unsupported feature */
#define IDN_PLEV_MCLERR         0x0800u     /* minimum chunk length */
#define IDN_PLEV_CTYERR         0x0400u     /* continuity */
#define IDN_PLEV_DCMERR         0x0200u     /* SCM/SDM mismatch */
#define IDN_PLEV_CKTERR         0x0100u     /* chunk type */
#define IDN_PLEV_CFGERR         0x0080u     /* service mode configuration */
#define IDN_PLEV_FRGERR         0x0040u     /* fragment reassembly */
#define IDN_PLEV_BSYERR         0x0020u     /* service busy */
#define IDN_PLEV_SMERR          0x0010u     /* invalid service ID / mode */
#define IDN_PLEV_CNLCLSD        0x0002u     /* channel closed: static, never latched */
#define IDN_PLEV_CNLRTED        0x0001u     /* channel routed */

/* -- ACK status flags (Hello 6.3.3) ----------------------------------------- */
#define IDN_STATUS_SNMSG        0x04u       /* session keeps queued messages */
#define IDN_STATUS_DOBUF        0x02u       /* devices occupy buffers */
#define IDN_STATUS_SOCNL        0x01u       /* session has open channels */

/* -- Scan response status flags (Hello 4.1.2) ------------------------------- */
#define IDN_SCAN_STATUS_MALFN   0x80u
#define IDN_SCAN_STATUS_OFFLINE 0x40u
#define IDN_SCAN_STATUS_XCLD    0x20u
#define IDN_SCAN_STATUS_OCPD    0x10u
#define IDN_SCAN_STATUS_RT      0x01u

#define IDN_ACK_STRUCT_SIZE     12u

/* ========================================================================== */
/* RT packet sequence (Hello 6)                                               */
/* ========================================================================== */

/*
 * Project policy:
 * - The first observed packet sets the baseline. Only IDN-RT packets
 *   (0x40/0x41/0x44/0x45, including empty ones) are observed; management
 *   requests have their own sequence numbers and must not be passed here.
 * - Serial arithmetic modulo 2^16. A forward distance of 1..0x7FFF is new
 *   traffic; 0x8000 is ambiguous and is treated as older (not accepted).
 * - A window of the last IDN_SEQ_WINDOW sequence numbers tracks which were
 *   seen. A repeat inside it is a duplicate; an unseen older number inside
 *   it is late. There is no reordering queue: late packets are reported and
 *   dropped, so old artwork is never replayed after newer artwork.
 * - Missing numbers (SeqErr type 3) are counted only when they leave the
 *   window unseen, so a late arrival is never counted lost as well, and a
 *   wrap from 65535 to 0 is ordinary +1 progress.
 * - Anything older than the window is stale and dropped. A run of
 *   IDN_SEQ_RESYNC_RUN consecutive stale numbers that count up by one
 *   re-baselines, so a sender that restarted its counter is not locked out.
 * - Rejected packets (duplicate, late, stale) have accept == false. Callers
 *   must not treat them as accepted RT traffic: they do not refresh the
 *   link timeout and their payload is not processed.
 * - Type 1 (not strictly +1) is set with every irregularity; types 2 and 3
 *   refine it.
 */
#define IDN_SEQ_WINDOW          32u
#define IDN_SEQ_RESYNC_RUN      4u

typedef struct {
    bool     valid;
    uint8_t  span;          /* window positions at or after the baseline, 1..32 */
    uint8_t  stale_run;
    uint16_t highest;       /* newest accepted sequence number */
    uint16_t stale_next;    /* stale number that would extend the run */
    uint32_t seen;          /* bit k set: (highest - k) was received */
} idn_seq_tracker_t;

typedef enum {
    IDN_SEQ_FIRST = 0,      /* baseline established */
    IDN_SEQ_NEXT,           /* highest + 1 */
    IDN_SEQ_GAP,            /* forward jump over unseen numbers */
    IDN_SEQ_DUPLICATE,      /* already received */
    IDN_SEQ_LATE,           /* older, unseen, inside the window: dropped */
    IDN_SEQ_STALE,          /* older than the window or ambiguous: dropped */
    IDN_SEQ_RESYNC,         /* stale run accepted as a new baseline */
} idn_seq_class_t;

typedef struct {
    idn_seq_class_t cls;
    bool     accept;        /* process the payload and refresh the link */
    uint16_t input_events;  /* IDN_INEV_SEQ_* bits to latch */
    uint32_t missing;       /* numbers declared lost by this observation */
} idn_seq_result_t;

void idn_seq_reset(idn_seq_tracker_t *t);
idn_seq_result_t idn_seq_observe(idn_seq_tracker_t *t, uint16_t seq);

/* ========================================================================== */
/* Message timestamp chronology (Stream 2.1, Hello 6)                         */
/* ========================================================================== */

/*
 * 32-bit microsecond timestamps are unwrapped onto a 64-bit timeline by
 * taking the signed 32-bit difference to the last accepted value: forward
 * steps up to 2^31 - 1 us (about 35.8 minutes) are forward, anything else
 * is earlier. Equal timestamps are legal. Timestamps of sequel fragments
 * (chunk types 0xC0..0xFF) are fragment numbers and must not be passed in;
 * see idn_chunk_has_timestamp().
 *
 * Check and commit are separate so a message rejected for another reason
 * can be left out of the baseline deliberately.
 */
typedef struct {
    bool     valid;
    uint64_t last_us;       /* unwrapped timestamp of the last accepted message */
} idn_chronology_t;

typedef enum {
    IDN_TS_FIRST = 0,
    IDN_TS_FORWARD,
    IDN_TS_EQUAL,
    IDN_TS_BEFORE,          /* earlier than an accepted message: BPErr, drop */
} idn_ts_order_t;

bool idn_chunk_has_timestamp(uint8_t chunk_type);
/* Unwrap a wire timestamp against an unwrapped reference. */
uint64_t idn_ts_unwrap(uint64_t reference_us, uint32_t wire_us);
void idn_chronology_reset(idn_chronology_t *c);
idn_ts_order_t idn_chronology_check(const idn_chronology_t *c, uint32_t wire_us,
                                    uint64_t *unwrapped_us);
void idn_chronology_commit(idn_chronology_t *c, uint64_t unwrapped_us);

/* ========================================================================== */
/* Per-channel wave continuity (Stream 6.1)                                   */
/* ========================================================================== */

/*
 * The timestamp of a wave chunk plus its duration must equal the timestamp
 * of the next wave chunk on the channel. Void and configuration-only
 * messages do not advance this timeline.
 *
 * Project policy for a discontinuity: report CTYErr, drop the chunk and end
 * the continuous epoch (the caller blanks). The channel then stays BROKEN,
 * and further wave chunks are dropped, until the caller passes a recovery
 * boundary: an accepted configuration message on the channel, including an
 * unchanged repeated one (senders repeat configuration every 200 ms), or a
 * new route/connection generation. The next wave chunk then starts a new
 * epoch (IDN_WAVE_RESTART). A repeated configuration on a CONTINUOUS
 * channel changes nothing.
 */
typedef enum {
    IDN_WAVE_IDLE = 0,      /* no epoch; next wave chunk starts one */
    IDN_WAVE_CONTINUOUS,
    IDN_WAVE_BROKEN,        /* discontinuity seen; waiting for recovery */
} idn_wave_state_t;

typedef struct {
    idn_wave_state_t state;
    bool     had_epoch;     /* an epoch ended since the last reset */
    uint64_t next_us;       /* expected unwrapped timestamp of the next chunk */
} idn_wave_timeline_t;

typedef enum {
    IDN_WAVE_START = 0,     /* first epoch since reset */
    IDN_WAVE_RESTART,       /* new epoch after an earlier one ended */
    IDN_WAVE_CONTINUES,     /* exactly continuous */
    IDN_WAVE_GAP,           /* later than expected: CTYErr */
    IDN_WAVE_OVERLAP,       /* earlier than expected: CTYErr */
    IDN_WAVE_AWAIT_RECOVERY,/* BROKEN: CTYErr, drop until recovery boundary */
} idn_wave_verdict_t;

void idn_wave_reset(idn_wave_timeline_t *w);
idn_wave_verdict_t idn_wave_check(const idn_wave_timeline_t *w, uint64_t ts_us);
void idn_wave_commit(idn_wave_timeline_t *w, uint64_t ts_us, uint32_t duration_us);
/* End the epoch after a discontinuity or rejected chunk. */
void idn_wave_break(idn_wave_timeline_t *w);
/* Recovery boundary (accepted configuration): BROKEN -> IDLE, else no-op. */
void idn_wave_recover(idn_wave_timeline_t *w);
/* Close the current epoch without error (e.g. output stopped): -> IDLE. */
void idn_wave_end(idn_wave_timeline_t *w);
bool idn_wave_verdict_is_error(idn_wave_verdict_t v);

/* ========================================================================== */
/* Sample-rate policy                                                         */
/* ========================================================================== */

/*
 * A wave chunk of n samples over d microseconds asks for n * 1e6 / d Hz.
 * Senders quantize durations to whole microseconds, so with cumulative
 * rounding each chunk's ideal duration lies within d +/- resolution_us, and
 * the summed duration of a continuous epoch within D +/- resolution_us. A
 * rate is judged by that envelope, never by a single rounded quotient:
 *
 *   - n == 0 or d == 0: INVALID.
 *   - Chunk envelope entirely below min_hz: BELOW_MIN.
 *   - Lower bound of the epoch including this chunk, (N + n) * 1e6 /
 *     (D + d + resolution), above limit_hz: ABOVE_LIMIT. A 30 kpps stream
 *     quantized to 2133/2134 us per 64 samples passes a 30 kpps limit; a
 *     30,100 pps or 50 kpps stream, or a constant 2133 us (30,004.7 pps)
 *     sustained for a few chunks, does not. The limit is never raised.
 *   - Chunk envelope disjoint from the epoch envelope, or (once the output
 *     rate is locked) not within 1 Hz of the locked rate: CHANGED.
 *
 * Project policy (first-release fallback): one stable rate per continuous
 * output run. The output rate is selected once, before output starts, from
 * the epoch estimate (idn_rate_select) and is never changed at receive time,
 * because a RATE command retimes every sample already queued. A genuine
 * in-stream rate change is rejected (RGUErr) instead of being played at the
 * old rate or rescheduled. Quantization alternation is not a rate change.
 */
typedef struct {
    uint32_t min_hz;
    uint32_t limit_hz;      /* scanner safety limit: authoritative */
    uint32_t resolution_us; /* timestamp quantization envelope, normally 1 */
} idn_rate_policy_t;

typedef struct {
    uint64_t samples;       /* N: samples accepted in the epoch */
    uint64_t duration_us;   /* D: their summed duration */
} idn_rate_epoch_t;

typedef enum {
    IDN_RATE_OK = 0,
    IDN_RATE_INVALID,
    IDN_RATE_BELOW_MIN,
    IDN_RATE_ABOVE_LIMIT,
    IDN_RATE_CHANGED,
} idn_rate_verdict_t;

void idn_rate_reset(idn_rate_epoch_t *e);
/* locked_hz: output rate already applied for this run, or 0 if none yet. */
idn_rate_verdict_t idn_rate_check(const idn_rate_policy_t *p, const idn_rate_epoch_t *e,
                                  uint32_t samples, uint32_t duration_us,
                                  uint32_t locked_hz);
void idn_rate_commit(idn_rate_epoch_t *e, uint32_t samples, uint32_t duration_us);
/* Nearest integer rate for the epoch, clamped to [min_hz, limit_hz]; 0 if empty. */
uint32_t idn_rate_select(const idn_rate_policy_t *p, const idn_rate_epoch_t *e);
/* Requested epoch rate in millihertz, for diagnostics; 0 if empty. */
uint64_t idn_rate_requested_mhz(const idn_rate_epoch_t *e);

/* ========================================================================== */
/* Latched ACK events (Hello 6.3.3)                                           */
/* ========================================================================== */

/*
 * One latch per scope: one for the link/session input events, one per
 * channel for pipeline events. Events stay latched until reported:
 *
 *   rep = idn_event_latch_snapshot(&latch);   // inside the caller's lock
 *   ... build and send the ACK with rep.flags ...
 *   if (sent) idn_event_latch_reported(&latch, &rep);   // inside the lock
 *
 * An event recorded between snapshot and reported() stays latched even if
 * the same bit was reported. A stale report (an older snapshot than the
 * latest) clears nothing, so events are over-reported rather than lost.
 * CnlClsd is static state and must not be recorded here.
 */
typedef struct {
    uint16_t flags;
    uint16_t since_snapshot;
    uint32_t generation;
} idn_event_latch_t;

typedef struct {
    uint16_t flags;
    uint32_t generation;
} idn_event_report_t;

void idn_event_latch_reset(idn_event_latch_t *l);
void idn_event_latch_record(idn_event_latch_t *l, uint16_t bits);
idn_event_report_t idn_event_latch_snapshot(idn_event_latch_t *l);
void idn_event_latch_reported(idn_event_latch_t *l, const idn_event_report_t *r);

/* ========================================================================== */
/* ACK and scan serialization                                                 */
/* ========================================================================== */

/*
 * Latency and link quality: 0 means unknown (Hello 6.3.3). This firmware
 * has no measured estimate for either, so callers leave them 0.
 */
typedef struct {
    uint8_t  result_code;
    uint16_t input_event_flags;
    uint16_t pipeline_event_flags;
    uint8_t  status_flags;
    uint8_t  link_quality;
    uint32_t latency_us;
} idn_ack_fields_t;

/* Write the 12-octet acknowledgement header, big-endian. Returns 12. */
size_t idn_ack_serialize(const idn_ack_fields_t *f, uint8_t out[IDN_ACK_STRUCT_SIZE]);

/*
 * Pipeline flags for the channel of the acknowledged message. With no
 * message (empty RT packet) there is no channel: returns 0 (reset value).
 * CnlClsd is recomputed from channel_closed and never taken from the latch.
 */
uint16_t idn_ack_pipeline_flags(uint16_t latched, bool has_channel, bool channel_closed);

/*
 * device_may_hold_data must be true whenever the device side might still
 * hold stream data and no completion point proves otherwise.
 */
uint8_t idn_ack_status_flags(bool open_channels, bool session_has_messages,
                             bool device_may_hold_data);

typedef struct {
    bool     rt_supported;
    uint32_t sessions_total;
    uint32_t sessions_in_use;
    bool     group_excluded;
    bool     offline;       /* only when known */
    bool     malfunction;   /* only when known */
} idn_scan_state_t;

uint8_t idn_scan_status(const idn_scan_state_t *s);
