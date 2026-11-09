#pragma once

/*
 * idn_session.h
 *
 * IDN-RT connection, channel and route state for one allocatable session.
 * Pure C: no sockets, FreeRTOS, clocks or DAC access. The server passes in
 * the time and the datagram; every output action (decoder build, feed start,
 * sample enqueue, cancellation) goes through idn_session_ops_t.
 *
 * Sources: ILDA IDN-Stream rev002 (2.1, 2.2, 3.2, 6.7) and the IDN-Hello
 * 2022-03-27 draft (3.2, 6, 7). Project policy, not spec requirement:
 * one session, one continuous laser route, discrete/effects/DMX/audio modes
 * rejected with SMErr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "idn_timing.h"

#define IDN_SESSION_CHANNELS        64
#define IDN_SESSION_LINK_TIMEOUT_US 1000000LL   /* Hello 7.2 default */
#define IDN_SESSION_CFG_MAX_BYTES   (255u * 4u) /* SCWC is one octet of 32-bit words */
#define IDN_SESSION_LASER_SERVICE   1           /* the one advertised service ID */
#define IDN_SESSION_AUTH_BYTES      12

/* IDN-Hello commands handled by the session */
#define IDN_CMD_GROUP_REQ           0x0C
#define IDN_CMD_GROUP_RESP          0x0D
#define IDN_CMD_RT_CHANNEL          0x40
#define IDN_CMD_RT_CHANNEL_ACK      0x41
#define IDN_CMD_RT_CLOSE            0x44
#define IDN_CMD_RT_CLOSE_ACK        0x45
#define IDN_CMD_RT_ABORT            0x46

/* IDN-Stream chunk types and service modes */
#define IDN_CHUNK_VOID              0x00
#define IDN_CHUNK_WAVE              0x01
#define IDN_CHUNK_SEQUEL_MIN        0xC0
#define IDN_MODE_VOID               0x00
#define IDN_MODE_GRAPHIC_CONTINUOUS 0x01

/* Acknowledge result codes and event/status flags (Hello 6.3.3): the
   session's names for the values defined once in idn_timing.h. */
#define IDN_RESULT_OK               IDN_ACK_RESULT_OK
#define IDN_RESULT_EMPTY_CLOSE      IDN_ACK_RESULT_EMPTY_CLOSE
#define IDN_RESULT_OCCUPIED         IDN_ACK_RESULT_OCCUPIED
#define IDN_RESULT_EXCLUDED         IDN_ACK_RESULT_EXCLUDED
#define IDN_RESULT_INVALID          IDN_ACK_RESULT_INVALID_PAYLOAD
#define IDN_RESULT_OTHER            IDN_ACK_RESULT_OTHER_ERROR

/* Input event flags */
#define IDN_IEV_NEW                 IDN_INEV_NEW
#define IDN_IEV_MVERR               IDN_INEV_MVERR
#define IDN_IEV_BPERR               IDN_INEV_BPERR
#define IDN_IEV_CCERR               IDN_INEV_CCERR
#define IDN_IEV_IRAERR              IDN_INEV_IRAERR

/* Pipeline event flags */
#define IDN_PEV_CNLRTED             IDN_PLEV_CNLRTED
#define IDN_PEV_CNLCLSD             IDN_PLEV_CNLCLSD    /* static: recomputed from state */
#define IDN_PEV_SMERR               IDN_PLEV_SMERR
#define IDN_PEV_BSYERR              IDN_PLEV_BSYERR
#define IDN_PEV_FRGERR              IDN_PLEV_FRGERR
#define IDN_PEV_CFGERR              IDN_PLEV_CFGERR
#define IDN_PEV_CKTERR              IDN_PLEV_CKTERR
#define IDN_PEV_DCMERR              IDN_PLEV_DCMERR
#define IDN_PEV_CTYERR              IDN_PLEV_CTYERR
#define IDN_PEV_MCLERR              IDN_PLEV_MCLERR
#define IDN_PEV_RGUERR              IDN_PLEV_RGUERR
#define IDN_PEV_PVLERR              IDN_PLEV_PVLERR
#define IDN_PEV_DVIERR              IDN_PLEV_DVIERR
#define IDN_PEV_IAPERR              IDN_PLEV_IAPERR

/* Status flags: IDN_STATUS_SOCNL (set here), IDN_STATUS_SNMSG and
   IDN_STATUS_DOBUF (added by the server, which owns the queues). */

/* Client group response codes (Hello 3.2.2) */
#define IDN_GROUP_OK                0x00
#define IDN_GROUP_AUTH_ERROR        0xFD
#define IDN_GROUP_UNSUPPORTED       0xFE
#define IDN_GROUP_INVALID           0xFF
#define IDN_GROUP_REQ_BYTES         16
#define IDN_GROUP_RESP_BYTES        4

/*
   Connection identity. Addresses are opaque (the server stores them in
   network order). local_addr is the destination address the datagram was
   received on; 0 means the server could not tell (wildcard socket without
   packet info), so links on different interfaces then compare equal.
*/
typedef struct {
    uint32_t peer_addr;
    uint16_t peer_port;
    uint32_t local_addr;
} idn_endpoint_t;

typedef enum {
    IDN_SESSION_FREE = 0,
    IDN_SESSION_ACTIVE,
    IDN_SESSION_CLOSING,
    IDN_SESSION_FAULTED,    /* output never confirmed quiescent; not allocatable */
} idn_session_state_t;

typedef enum {
    IDN_CHANNEL_CLOSED = 0,
    IDN_CHANNEL_VOIDED,         /* routed to no service: no output, no CCErr */
    IDN_CHANNEL_ROUTED_IDLE,    /* route/configuration accepted, no data yet */
    IDN_CHANNEL_ROUTED_ACTIVE,  /* data queued or playing under this route */
} idn_channel_state_t;

typedef enum {
    IDN_ROUTE_NONE = 0,
    IDN_ROUTE_VOID_MODE,        /* test mode: data chunks ignored */
    IDN_ROUTE_LASER_CONTINUOUS,
} idn_route_t;

typedef enum {
    IDN_VOID_NONE = 0,
    IDN_VOID_INVALID_SERVICE,
    IDN_VOID_BUSY,
    IDN_VOID_CONFIG,
    IDN_VOID_SCM_MISMATCH,
} idn_void_reason_t;

/* Decoder build result, returned by the decoder adapter (plan 01). */
typedef enum {
    IDN_CFG_OK = 0,
    IDN_CFG_INVALID,
    IDN_CFG_UNSUPPORTED,
    IDN_CFG_NO_RESOURCES,
} idn_cfg_result_t;

/* Chunk check / enqueue result. */
typedef enum {
    IDN_CHUNK_OK = 0,
    IDN_CHUNK_TOO_SHORT,        /* MCLErr */
    IDN_CHUNK_PAYLOAD_INVALID,  /* PVLErr */
    IDN_CHUNK_UNSUPPORTED,      /* RGUErr */
    IDN_CHUNK_DEVICE_OVERRUN,   /* DVIErr: samples dropped downstream */
    IDN_CHUNK_INTERNAL,         /* IAPErr */
} idn_chunk_result_t;

typedef enum {
    IDN_STOP_CHANNEL_CLOSE = 0,
    IDN_STOP_ROUTE_CHANGE,      /* reroute, failed reconfiguration or SCM mismatch */
    IDN_STOP_GRACEFUL,          /* 0x44/0x45 connection close */
    IDN_STOP_ABORT,             /* 0x46 */
    IDN_STOP_TIMEOUT,
    IDN_STOP_EXCLUDED,          /* client group mask change */
    IDN_STOP_CANCEL,            /* local user Stop / takeover */
} idn_stop_reason_t;

/*
   Output and decoder actions. Required: config_compile, wave_check,
   output_acquire, wave_enqueue, output_release, output_quiescent.
   config_discard, rate_limits and wave_epoch may be NULL.
*/
typedef struct {
    void *ctx;

    /* Build a decoder for channel from the service configuration bytes.
       On failure the channel's decoder may be left invalid (never a
       fallback); the session voids the channel either way. */
    idn_cfg_result_t (*config_compile)(void *ctx, uint8_t channel, uint8_t mode,
                                       const uint8_t *cfg, uint16_t cfg_len);
    /* The channel's decoder is void; drop it. */
    void (*config_discard)(void *ctx, uint8_t channel);

    /* Validate a wave chunk (header + samples) against the channel's decoder
       without side effects; *samples receives its sample count. */
    idn_chunk_result_t (*wave_check)(void *ctx, uint8_t channel,
                                     const uint8_t *chunk, uint16_t chunk_len,
                                     uint32_t *samples);

    /* Take the physical output for generation gen. false: unavailable. */
    bool (*output_acquire)(void *ctx, uint32_t gen);
    /* Enqueue a checked chunk under output generation gen. */
    idn_chunk_result_t (*wave_enqueue)(void *ctx, uint8_t channel, uint32_t gen,
                                       const uint8_t *chunk, uint16_t chunk_len);
    /* Cancel output generation gen and wait (bounded) for the producer to
       stop. true: quiescent and released. false: deadline missed. */
    bool (*output_release)(void *ctx, uint32_t gen, idn_stop_reason_t reason);
    /* After a missed release deadline: has the producer stopped since? */
    bool (*output_quiescent)(void *ctx);

    /* Plan 03. rate_limits: the scanner rate policy, and the rate already
       applied to output generation gen (0 = gen not held or none locked).
       NULL: wave chunk rates are not checked. */
    void (*rate_limits)(void *ctx, uint32_t gen, idn_rate_policy_t *policy,
                        uint32_t *locked_hz);
    /* Plan 03. A continuous epoch on the channel under output generation gen
       starts with the next enqueue (start), or has just ended (!start): the
       server blanks across the boundary. */
    void (*wave_epoch)(void *ctx, uint8_t channel, uint32_t gen, bool start);
} idn_session_ops_t;

/* One parsed IDN-Stream channel message (pointers into the datagram). */
typedef struct {
    bool           channel_msg;     /* content ID 0x8000..0xFFFF */
    uint8_t        channel_id;
    uint8_t        chunk_type;
    uint32_t       timestamp;
    bool           has_cfg;
    bool           routing;
    bool           close;
    uint8_t        sdm;
    uint8_t        service_id;      /* valid only with routing */
    uint8_t        service_mode;    /* valid only with routing */
    const uint8_t *cfg;
    uint16_t       cfg_len;
    const uint8_t *data;
    uint16_t       data_len;
    bool           cfg_truncated;   /* parse failed: SCWC region cut short */
} idn_msg_t;

typedef struct {
    idn_channel_state_t state;
    idn_route_t         route;
    idn_void_reason_t   void_reason;
    bool                decoder_valid;
    uint8_t             sdm;
    uint32_t            route_gen;
    uint32_t            config_gen;
    idn_event_latch_t   pipeline;           /* pipeline events since the last sent ACK */
    idn_wave_timeline_t wave;               /* plan 03: wave continuity */
    idn_rate_epoch_t    rate;               /* plan 03: rate of the current epoch */
} idn_channel_t;

/* Plan 03 receiver counters, since boot; written by the session only. */
typedef struct {
    uint32_t rt_accepted;
    uint32_t malformed;
    uint32_t seq_duplicates;
    uint32_t seq_late;
    uint32_t seq_stale;
    uint32_t seq_missing;
    uint32_t seq_resyncs;
    uint32_t ts_before;
    uint32_t ts_resyncs;
    uint32_t continuity_errors;
    uint32_t rate_rejects;
    uint32_t chunk_rejects;
} idn_session_counters_t;

typedef struct {
    idn_session_ops_t   ops;

    /* Server-wide: client group mask and its set code (Hello 3.2). */
    uint16_t            group_mask;
    uint8_t             group_auth[IDN_SESSION_AUTH_BYTES];

    idn_session_state_t state;
    idn_endpoint_t      peer;
    uint16_t            groups_seen;        /* groups accepted on this connection */
    int64_t             last_rt_us;
    uint32_t            conn_gen;
    idn_event_latch_t   input;              /* input events since the last sent ACK */
    idn_seq_tracker_t   seq;                /* plan 03: RT sequence of this connection */
    idn_chronology_t    chrono;             /* plan 03: message timestamps */

    int                 laser_channel;      /* channel holding the continuous route, -1 none */
    uint16_t            laser_cfg_len;
    uint8_t             laser_cfg[IDN_SESSION_CFG_MAX_BYTES];

    bool                output_held;
    bool                output_fault;       /* a release missed its deadline */
    uint32_t            output_gen;
    uint32_t            fault_count;

    idn_session_counters_t counters;

    idn_channel_t       ch[IDN_SESSION_CHANNELS];
} idn_session_t;

typedef struct {
    uint8_t  result;
    uint16_t input_events;
    uint16_t pipeline_events;
    uint8_t  status;
} idn_session_ack_t;

typedef struct {
    uint8_t           result;           /* also used for ACK */
    bool              send_ack;
    bool              accepted;         /* reached session processing */
    bool              connection_opened;
    bool              connection_closed;
    idn_session_ack_t ack;              /* valid if send_ack */

    /* Latch snapshots behind ack, for idn_session_ack_sent(). */
    bool               latched;
    uint32_t           conn_gen;
    int                ack_channel;     /* -1: no channel fields */
    idn_event_report_t input_report;
    idn_event_report_t pipeline_report;
} idn_rt_outcome_t;

void idn_session_init(idn_session_t *s, const idn_session_ops_t *ops);

/*
   Bounds-check one channel message. Requires total size == len (0x0008..
   0xFF00), a complete configuration header and SCWC region, SCWC 0 when
   Routing is clear, and no data after a Void chunk. Never clamps. When the
   SCWC region is cut short, cfg_truncated and channel_id are still set.
*/
bool idn_session_parse_message(const uint8_t *p, size_t len, idn_msg_t *m);

/* Handle an IDN-RT packet (0x40, 0x41, 0x44, 0x45, 0x46) received at now_us. */
void idn_session_rt(idn_session_t *s, int64_t now_us, const idn_endpoint_t *from,
                    uint8_t cmd, uint8_t flags, uint16_t seq,
                    const uint8_t *payload, size_t payload_len,
                    idn_rt_outcome_t *out);

/*
   The ACK built from out was sent. Only now are the events it reported
   cleared; an event latched since then, or a lost ACK, stays reported.
*/
void idn_session_ack_sent(idn_session_t *s, const idn_rt_outcome_t *out);

/* Link timeout and fault recovery; call at least every 100 ms. */
void idn_session_tick(idn_session_t *s, int64_t now_us);

/* Close the connection and its output for a local reason (user Stop). */
void idn_session_cancel(idn_session_t *s, idn_stop_reason_t reason);

/*
   Client group request (0x0C). resp receives the 4-octet response header.
   Excluding a group the active connection has used closes the connection.
*/
void idn_session_group_request(idn_session_t *s, const uint8_t *payload,
                               size_t payload_len,
                               uint8_t resp[IDN_GROUP_RESP_BYTES]);

/* Set the group-mask code; len <= IDN_SESSION_AUTH_BYTES, zero padded. */
bool idn_session_set_group_auth(idn_session_t *s, const uint8_t *code, size_t len);

bool idn_session_occupied(const idn_session_t *s);
bool idn_session_group_excluded(const idn_session_t *s, uint8_t group);
