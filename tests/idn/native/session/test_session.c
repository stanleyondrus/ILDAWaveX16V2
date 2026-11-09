/*
 * Host tests for firmware/ESP32S3/main/idn_session.c.
 *
 * Each scenario is a list of steps: an input (RT packet, clock tick, group
 * request, local cancel) with literal expectations for the result code,
 * session/channel state, ACK event flags and the exact output actions the
 * step caused. The fake output sink also checks the ownership contract:
 * enqueue only under the generation it acquired, release only what it holds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idn_session.h"

/* -- Fake decoder/output sink ------------------------------------------------ */

typedef struct {
    int compiles;
    int discards;
    int acquires;
    int enqueues;
    int releases;
    int violations;

    bool acquire_ok;
    bool release_ok;
    bool quiescent;

    bool held;
    uint32_t held_gen;
    uint32_t last_gen;
    idn_stop_reason_t last_reason;
    bool decoder[IDN_SESSION_CHANNELS];
} fake_t;

static fake_t g_fake;

static idn_cfg_result_t fake_compile(void *ctx, uint8_t ch, uint8_t mode,
                                     const uint8_t *cfg, uint16_t len)
{
    fake_t *f = ctx;
    f->compiles++;
    if (mode != IDN_MODE_GRAPHIC_CONTINUOUS) {
        f->violations++;
    }
    /* Tag 0xFFFF stands in for any configuration the decoder rejects. */
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        if (cfg[i] == 0xFF && cfg[i + 1] == 0xFF) {
            return IDN_CFG_INVALID;
        }
    }
    f->decoder[ch] = true;
    return IDN_CFG_OK;
}

static void fake_discard(void *ctx, uint8_t ch)
{
    fake_t *f = ctx;
    f->discards++;
    f->decoder[ch] = false;
}

static idn_chunk_result_t fake_check(void *ctx, uint8_t ch, const uint8_t *chunk, uint16_t len)
{
    fake_t *f = ctx;
    (void)chunk;
    if (!f->decoder[ch]) {
        f->violations++;
    }
    if (len < 4 + 20 * 8 || (len - 4) % 8 != 0) {
        return IDN_CHUNK_TOO_SHORT;
    }
    return IDN_CHUNK_OK;
}

static bool fake_acquire(void *ctx, uint32_t gen)
{
    fake_t *f = ctx;
    f->acquires++;
    if (f->held || gen <= f->last_gen) {
        f->violations++;
    }
    if (!f->acquire_ok) {
        return false;
    }
    f->held = true;
    f->held_gen = gen;
    f->last_gen = gen;
    return true;
}

static idn_chunk_result_t fake_enqueue(void *ctx, uint8_t ch, uint32_t gen,
                                       const uint8_t *chunk, uint16_t len)
{
    fake_t *f = ctx;
    (void)chunk;
    (void)len;
    f->enqueues++;
    if (!f->held || gen != f->held_gen || !f->decoder[ch]) {
        f->violations++;
    }
    return IDN_CHUNK_OK;
}

static bool fake_release(void *ctx, uint32_t gen, idn_stop_reason_t reason)
{
    fake_t *f = ctx;
    f->releases++;
    f->last_reason = reason;
    if (!f->held || gen != f->held_gen) {
        f->violations++;
    }
    f->held = false;
    return f->release_ok;
}

static bool fake_quiescent(void *ctx)
{
    fake_t *f = ctx;
    return f->quiescent;
}

static const idn_session_ops_t k_ops = {
    .ctx = &g_fake,
    .config_compile = fake_compile,
    .config_discard = fake_discard,
    .wave_check = fake_check,
    .output_acquire = fake_acquire,
    .wave_enqueue = fake_enqueue,
    .output_release = fake_release,
    .output_quiescent = fake_quiescent,
};

/* -- Packet builder --------------------------------------------------------- */

typedef struct {
    bool    present;
    bool    not_channel_msg;
    uint8_t ch;
    uint8_t chunk;
    bool    cfg;
    uint8_t cfl;
    uint8_t sid;
    uint8_t mode;
    uint8_t scwc;
    bool    bad_cfg;        /* decoder will reject */
    bool    alt_cfg;        /* different but valid dictionary */
    uint8_t nsamples;
    uint8_t scm;
    int     size_delta;     /* corrupt the declared total size */
    uint8_t scwc_extra;     /* declare more configuration words than sent */
} msg_spec_t;

static size_t build_msg(const msg_spec_t *m, uint8_t *buf)
{
    if (!m->present) {
        return 0;
    }

    /* XYRGBI dictionary from plan 04: 8 tags, SCWC 4 */
    static const uint16_t dict[8] = {
        0x4200, 0x4010, 0x4210, 0x4010, 0x527E, 0x5214, 0x51CC, 0x5C10,
    };

    size_t n = 8;
    buf[2] = (uint8_t)((m->not_channel_msg ? 0x00 : 0x80) | (m->cfg ? 0x40 : 0) | (m->ch & 0x3F));
    buf[3] = m->chunk;
    buf[4] = 0x00; buf[5] = 0x01; buf[6] = 0x00; buf[7] = 0x00;

    if (m->cfg) {
        buf[n++] = (uint8_t)(m->scwc + m->scwc_extra);
        buf[n++] = m->cfl;
        buf[n++] = m->sid;
        buf[n++] = m->mode;
        for (unsigned w = 0; w < m->scwc * 2u; w++) {
            uint16_t tag = dict[w % 8];
            if (m->bad_cfg && w == 0) tag = 0xFFFF;
            if (m->alt_cfg && w == 7) tag = 0x0000;
            buf[n++] = (uint8_t)(tag >> 8);
            buf[n++] = (uint8_t)tag;
        }
    }

    if (m->chunk == IDN_CHUNK_WAVE) {
        uint32_t dur = m->nsamples * 33u;
        buf[n++] = (uint8_t)((m->scm & 3) << 4);
        buf[n++] = (uint8_t)(dur >> 16);
        buf[n++] = (uint8_t)(dur >> 8);
        buf[n++] = (uint8_t)dur;
        memset(buf + n, 0, (size_t)m->nsamples * 8u);
        n += (size_t)m->nsamples * 8u;
    }

    size_t declared = (size_t)((int)n + m->size_delta);
    buf[0] = (uint8_t)(declared >> 8);
    buf[1] = (uint8_t)declared;
    return n;
}

#define ROUTING(sdm)    (uint8_t)(0x01 | ((sdm) << 4))
#define CLOSE           0x02

#define M_EMPTY                 { .present = false }
#define M_ROUTE(c, sdm)         { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, \
                                  .cfl = ROUTING(sdm), .sid = 1, .mode = 1, .scwc = 4 }
#define M_ROUTE_SVC(c, s, md)   { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, \
                                  .cfl = ROUTING(0), .sid = (s), .mode = (md), .scwc = 4 }
#define M_ROUTE_BAD(c)          { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, \
                                  .cfl = ROUTING(0), .sid = 1, .mode = 1, .scwc = 4, .bad_cfg = true }
#define M_ROUTE_ALT(c)          { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, \
                                  .cfl = ROUTING(0), .sid = 1, .mode = 1, .scwc = 4, .alt_cfg = true }
#define M_WAVE(c, sc)           { .present = true, .ch = (c), .chunk = IDN_CHUNK_WAVE, .nsamples = 20, .scm = (sc) }
#define M_ROUTE_WAVE(c, sdm)    { .present = true, .ch = (c), .chunk = IDN_CHUNK_WAVE, .cfg = true, \
                                  .cfl = ROUTING(sdm), .sid = 1, .mode = 1, .scwc = 4, .nsamples = 20, .scm = (sdm) }
#define M_CLOSE_CH(c)           { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, .cfl = CLOSE }

/* -- Scenario tables -------------------------------------------------------- */

enum { EP_A, EP_A_PORT2, EP_B, EP_A_IF1, EP_A_IF2, EP_COUNT };

static const idn_endpoint_t k_ep[EP_COUNT] = {
    [EP_A]       = { 0x0A000002u, 5000, 0 },
    [EP_A_PORT2] = { 0x0A000002u, 5001, 0 },
    [EP_B]       = { 0x0A000003u, 5000, 0 },
    [EP_A_IF1]   = { 0x0A000002u, 5000, 0xC0A80101u },
    [EP_A_IF2]   = { 0x0A000002u, 5000, 0x0A000001u },
};

typedef enum { EV_END = 0, EV_RT, EV_RT_RAW, EV_TICK, EV_GROUP, EV_CANCEL, EV_SET } ev_kind_t;

/* X(v): expect v. A zero field is not checked. Output deltas are always checked. */
#define X(v) ((v) + 1)

typedef struct {
    ev_kind_t  kind;
    int64_t    now;
    int        ep;
    uint8_t    cmd;
    uint8_t    group;
    msg_spec_t msg;
    const uint8_t *raw;      /* EV_RT_RAW / EV_GROUP payload */
    size_t     raw_len;

    /* EV_SET knobs: X(0)/X(1) */
    int set_acquire_ok;
    int set_release_ok;
    int set_quiescent;

    int exp_result;     /* RT result / group response code */
    int exp_session;
    int exp_ch;         /* channel whose state is checked */
    int exp_ch_state;
    int exp_iev;        /* ACK input events */
    int exp_pev;        /* ACK pipeline events */
    int exp_status;
    int exp_mask;       /* group response mask */
    int exp_reason;     /* last release reason */

    int d_compile, d_acquire, d_enqueue, d_release;
} step_t;

typedef struct {
    const char   *name;
    const step_t *steps;
} scenario_t;

#define RT(...)     { .kind = EV_RT, __VA_ARGS__ }
#define TICK(...)   { .kind = EV_TICK, __VA_ARGS__ }
#define GROUP(...)  { .kind = EV_GROUP, __VA_ARGS__ }
#define SET(...)    { .kind = EV_SET, __VA_ARGS__ }
#define END         { .kind = EV_END }

#define ACK     IDN_CMD_RT_CHANNEL_ACK
#define CLOSEACK IDN_CMD_RT_CLOSE_ACK
#define ABORT   IDN_CMD_RT_ABORT

/* Plan 02 transition table, row by row. */

static const step_t s_empty_allocates_only[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_ACTIVE),
       .exp_iev = X(IDN_IEV_NEW), .exp_pev = X(0), .exp_status = X(0)),
    RT(.ep = EP_A, .cmd = IDN_CMD_RT_CHANNEL, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_ACTIVE)),
    END
};

static const step_t s_route_void_idle[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE(0, 0),
       .exp_result = X(IDN_RESULT_OK), .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE),
       .exp_iev = X(IDN_IEV_NEW), .exp_pev = X(IDN_PEV_CNLRTED), .exp_status = X(IDN_STATUS_SOCNL),
       .d_compile = 1),
    END
};

static const step_t s_first_wave_acquires[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE(0, 0), .d_compile = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0),
       .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0), .exp_ch = X(0),
       .d_enqueue = 1),
    END
};

static const step_t s_no_auto_open[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(5, 0),
       .exp_result = X(IDN_RESULT_OK), .exp_ch = X(5), .exp_ch_state = X(IDN_CHANNEL_CLOSED),
       .exp_iev = X(IDN_IEV_NEW | IDN_IEV_CCERR), .exp_pev = X(IDN_PEV_CNLCLSD), .exp_status = X(0)),
    /* Void messages on a closed channel are not CCErr */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 5, .chunk = IDN_CHUNK_VOID },
       .exp_iev = X(0), .exp_pev = X(IDN_PEV_CNLCLSD)),
    END
};

static const step_t s_second_channel_busy[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(1, 0),
       .exp_ch = X(1), .exp_ch_state = X(IDN_CHANNEL_VOIDED),
       .exp_pev = X(IDN_PEV_BSYERR | IDN_PEV_CNLRTED)),
    /* Busy route is a NULL route: data is dropped without CCErr */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(1, 0),
       .exp_ch = X(1), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_iev = X(0), .exp_pev = X(0)),
    /* The first channel keeps playing */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .d_enqueue = 1),
    END
};

static const step_t s_unknown_service_mode[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(2, 7, 1),
       .exp_ch = X(2), .exp_ch_state = X(IDN_CHANNEL_VOIDED),
       .exp_pev = X(IDN_PEV_SMERR | IDN_PEV_CNLRTED), .exp_status = X(IDN_STATUS_SOCNL)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(2, 0),
       .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_ch = X(2), .exp_iev = X(0), .exp_pev = X(0)),
    /* Discrete graphics on the laser service: unsupported, SMErr */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(3, 1, 2),
       .exp_ch = X(3), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_SMERR | IDN_PEV_CNLRTED)),
    /* Manufacturer-specific mode, default service */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(4, 0, 0xC0),
       .exp_ch = X(4), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_SMERR | IDN_PEV_CNLRTED)),
    END
};

static const step_t s_scm_mismatch[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 1), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 2),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_DCMERR),
       .exp_reason = X(IDN_STOP_ROUTE_CHANGE), .d_release = 1),
    /* The stale decoder is gone, even for data matching the old SDM */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 1),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_CFGERR)),
    /* A new configuration recovers the channel */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE(0, 2),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_CNLRTED), .d_compile = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 2),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0),
       .d_acquire = 1, .d_enqueue = 1),
    END
};

static const step_t s_last_channel_close[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_CLOSE_CH(0),
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_ACTIVE),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_CLOSED),
       .exp_pev = X(IDN_PEV_CNLCLSD), .exp_status = X(0),
       .exp_reason = X(IDN_STOP_CHANNEL_CLOSE), .d_release = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_EMPTY,
       .exp_session = X(IDN_SESSION_ACTIVE), .exp_iev = X(0)),
    /* Reopen on the same connection */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_iev = X(0),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    END
};

static const step_t s_empty_close_no_connection[] = {
    RT(.ep = EP_A, .cmd = CLOSEACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_EMPTY_CLOSE), .exp_session = X(IDN_SESSION_FREE)),
    RT(.ep = EP_A, .cmd = IDN_CMD_RT_CLOSE, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_EMPTY_CLOSE), .exp_session = X(IDN_SESSION_FREE)),
    END
};

static const step_t s_same_ip_other_port[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A_PORT2, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_result = X(IDN_RESULT_OCCUPIED), .exp_session = X(IDN_SESSION_ACTIVE)),
    RT(.ep = EP_A_PORT2, .cmd = CLOSEACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_EMPTY_CLOSE), .exp_session = X(IDN_SESSION_ACTIVE)),
    RT(.ep = EP_A_PORT2, .cmd = ABORT, .msg = M_EMPTY, .exp_session = X(IDN_SESSION_ACTIVE)),
    RT(.ep = EP_B, .cmd = ACK, .msg = M_EMPTY, .exp_result = X(IDN_RESULT_OCCUPIED)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_iev = X(0), .d_enqueue = 1),
    END
};

static const step_t s_abort[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ABORT, .msg = M_EMPTY,
       .exp_session = X(IDN_SESSION_FREE), .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_CLOSED),
       .exp_reason = X(IDN_STOP_ABORT), .d_release = 1),
    /* Repeated abort does nothing */
    RT(.ep = EP_A, .cmd = ABORT, .msg = M_EMPTY, .exp_session = X(IDN_SESSION_FREE)),
    /* Data after abort opens a new connection on a closed channel */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_session = X(IDN_SESSION_ACTIVE), .exp_iev = X(IDN_IEV_NEW | IDN_IEV_CCERR)),
    END
};

static const step_t s_link_timeout[] = {
    RT(.now = 0, .ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.now = 500000, .ep = EP_A, .cmd = IDN_CMD_RT_CHANNEL, .msg = M_EMPTY),
    TICK(.now = 1499999, .exp_session = X(IDN_SESSION_ACTIVE)),
    TICK(.now = 1500000, .exp_session = X(IDN_SESSION_FREE), .exp_ch = X(0),
         .exp_ch_state = X(IDN_CHANNEL_CLOSED), .exp_reason = X(IDN_STOP_TIMEOUT), .d_release = 1),
    END
};

static const uint8_t k_group_get[16] = { 16, 0x01 };

static const step_t s_management_while_streaming[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    GROUP(.raw = k_group_get, .raw_len = sizeof(k_group_get),
          .exp_result = X(IDN_GROUP_OK), .exp_mask = X(0xFFFF), .exp_session = X(IDN_SESSION_ACTIVE)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_ch = X(0), .exp_iev = X(0), .d_enqueue = 1),
    END
};

/* Further cases from the plan's test list. */

static const step_t s_open_data_close_one_message[] = {
    RT(.ep = EP_A, .cmd = ACK,
       .msg = { .present = true, .ch = 9, .chunk = IDN_CHUNK_WAVE, .cfg = true, .cfl = ROUTING(0) | CLOSE,
                .sid = 1, .mode = 1, .scwc = 4, .nsamples = 20 },
       .exp_session = X(IDN_SESSION_ACTIVE), .exp_ch = X(9), .exp_ch_state = X(IDN_CHANNEL_CLOSED),
       .exp_pev = X(IDN_PEV_CNLRTED | IDN_PEV_CNLCLSD), .exp_status = X(0),
       .exp_reason = X(IDN_STOP_CHANNEL_CLOSE),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1, .d_release = 1),
    END
};

static const step_t s_repeated_config[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0), .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0)),
    /* A changed, valid dictionary rebuilds the decoder but keeps the route */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_ALT(0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0), .d_compile = 1),
    END
};

static const step_t s_failed_replacement[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_BAD(0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_CFGERR),
       .exp_reason = X(IDN_STOP_ROUTE_CHANGE), .d_compile = 1, .d_release = 1),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_CFGERR)),
    /* The voided channel no longer blocks another channel */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(1, 0),
       .exp_ch = X(1), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(IDN_PEV_CNLRTED),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    END
};

static const uint8_t k_group_set_no_g3[16] = { 16, 0x02, 0xFF, 0xF7 };
static const uint8_t k_group_set_bad_auth[16] = { 16, 0x02, 0xFF, 0xFF, 'x' };
static const uint8_t k_group_bad_op[16] = { 16, 0x07 };
static const uint8_t k_group_short[4] = { 4, 0x01 };

static const step_t s_group_exclusion[] = {
    RT(.ep = EP_A, .cmd = ACK, .group = 3, .msg = M_ROUTE_WAVE(0, 0),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    GROUP(.raw = k_group_set_bad_auth, .raw_len = 16,
          .exp_result = X(IDN_GROUP_AUTH_ERROR), .exp_mask = X(0xFFFF), .exp_session = X(IDN_SESSION_ACTIVE)),
    GROUP(.raw = k_group_bad_op, .raw_len = 16, .exp_result = X(IDN_GROUP_UNSUPPORTED)),
    GROUP(.raw = k_group_short, .raw_len = 4, .exp_result = X(IDN_GROUP_INVALID)),
    GROUP(.raw = k_group_set_no_g3, .raw_len = 16,
          .exp_result = X(IDN_GROUP_OK), .exp_mask = X(0xFFF7), .exp_session = X(IDN_SESSION_FREE),
          .exp_reason = X(IDN_STOP_EXCLUDED), .d_release = 1),
    RT(.ep = EP_A, .cmd = ACK, .group = 3, .msg = M_ROUTE_WAVE(0, 0),
       .exp_result = X(IDN_RESULT_EXCLUDED), .exp_session = X(IDN_SESSION_FREE)),
    RT(.ep = EP_A, .cmd = ACK, .group = 0, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_ACTIVE)),
    /* An excluded group on the live tuple is not processed either */
    RT(.ep = EP_A, .cmd = ACK, .group = 3, .msg = M_ROUTE(0, 0), .exp_result = X(IDN_RESULT_EXCLUDED)),
    END
};

static const step_t s_repeated_close[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = CLOSEACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_FREE), .exp_status = X(0),
       .exp_reason = X(IDN_STOP_GRACEFUL), .d_release = 1),
    RT(.ep = EP_A, .cmd = CLOSEACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_EMPTY_CLOSE), .exp_session = X(IDN_SESSION_FREE)),
    END
};

static const step_t s_close_with_message[] = {
    RT(.ep = EP_A, .cmd = CLOSEACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_result = X(IDN_RESULT_OK), .exp_session = X(IDN_SESSION_FREE),
       .exp_iev = X(IDN_IEV_NEW), .exp_pev = X(IDN_PEV_CNLRTED | IDN_PEV_CNLCLSD), .exp_status = X(0),
       .exp_reason = X(IDN_STOP_GRACEFUL),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1, .d_release = 1),
    END
};

static const step_t s_two_interfaces[] = {
    RT(.ep = EP_A_IF1, .cmd = ACK, .msg = M_EMPTY, .exp_result = X(IDN_RESULT_OK)),
    RT(.ep = EP_A_IF2, .cmd = ACK, .msg = M_EMPTY, .exp_result = X(IDN_RESULT_OCCUPIED)),
    RT(.ep = EP_A_IF1, .cmd = ACK, .msg = M_EMPTY, .exp_result = X(IDN_RESULT_OK), .exp_iev = X(0)),
    END
};

static const step_t s_malformed[] = {
    /* Declared size one more than received: no session */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 0, .chunk = 0, .size_delta = 1 },
       .exp_result = X(IDN_RESULT_INVALID), .exp_session = X(IDN_SESSION_FREE)),
    /* Trailing bytes beyond the declared size */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 0, .chunk = IDN_CHUNK_WAVE, .nsamples = 20, .size_delta = -8 },
       .exp_result = X(IDN_RESULT_INVALID), .exp_session = X(IDN_SESSION_FREE)),
    /* SCWC set without Routing */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 0, .chunk = 0, .cfg = true, .cfl = 0, .scwc = 4 },
       .exp_result = X(IDN_RESULT_INVALID), .exp_session = X(IDN_SESSION_FREE)),
    /* Not a channel message: well framed, reaches the session, MVErr */
    RT(.now = 0, .ep = EP_A, .cmd = ACK, .msg = { .present = true, .not_channel_msg = true },
       .exp_result = X(IDN_RESULT_OK), .exp_iev = X(IDN_IEV_NEW | IDN_IEV_MVERR)),
    /* A malformed packet does not refresh the link */
    RT(.now = 900000, .ep = EP_A, .cmd = ACK, .msg = { .present = true, .chunk = 0, .size_delta = 1 },
       .exp_result = X(IDN_RESULT_INVALID)),
    TICK(.now = 1000000, .exp_session = X(IDN_SESSION_FREE)),
    END
};

static const step_t s_release_deadline_fault[] = {
    SET(.set_release_ok = X(0), .set_quiescent = X(0)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    RT(.ep = EP_A, .cmd = ABORT, .msg = M_EMPTY, .exp_session = X(IDN_SESSION_FAULTED), .d_release = 1),
    RT(.ep = EP_B, .cmd = ACK, .msg = M_EMPTY,
       .exp_result = X(IDN_RESULT_OCCUPIED), .exp_session = X(IDN_SESSION_FAULTED)),
    TICK(.now = 2000000, .exp_session = X(IDN_SESSION_FAULTED)),
    SET(.set_quiescent = X(1), .set_release_ok = X(1)),
    TICK(.now = 2100000, .exp_session = X(IDN_SESSION_FREE)),
    RT(.now = 2100000, .ep = EP_B, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_result = X(IDN_RESULT_OK), .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    END
};

static const step_t s_output_busy_elsewhere[] = {
    SET(.set_acquire_ok = X(0)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE),
       .exp_pev = X(IDN_PEV_CNLRTED | IDN_PEV_BSYERR), .d_compile = 1, .d_acquire = 1),
    SET(.set_acquire_ok = X(1)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(0),
       .d_acquire = 1, .d_enqueue = 1),
    END
};

static const step_t s_void_mode[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(6, 0, IDN_MODE_VOID),
       .exp_ch = X(6), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_CNLRTED)),
    RT(.ep = EP_A, .cmd = ACK, .msg = M_WAVE(6, 0),
       .exp_ch = X(6), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_iev = X(0), .exp_pev = X(0)),
    /* A Void-mode channel does not hold the laser */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(7, 0),
       .exp_ch = X(7), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    /* Rerouting the laser channel to Void mode stops its output */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(7, 1, IDN_MODE_VOID),
       .exp_ch = X(7), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_CNLRTED),
       .exp_reason = X(IDN_STOP_ROUTE_CHANGE), .d_release = 1),
    END
};

static const step_t s_default_service_id[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_SVC(0, 0, IDN_MODE_GRAPHIC_CONTINUOUS),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_CNLRTED), .d_compile = 1),
    END
};

static const step_t s_wrong_chunk_type[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE(0, 0), .d_compile = 1),
    /* Frame samples on a continuous route */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 0, .chunk = 0x02, .nsamples = 0 },
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_CKTERR)),
    /* Under-minimum wave chunk: rejected before any output is taken */
    RT(.ep = EP_A, .cmd = ACK, .msg = { .present = true, .ch = 0, .chunk = IDN_CHUNK_WAVE, .nsamples = 19 },
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_IDLE), .exp_pev = X(IDN_PEV_MCLERR)),
    END
};

static const step_t s_user_cancel[] = {
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    { .kind = EV_CANCEL, .exp_session = X(IDN_SESSION_FREE), .exp_reason = X(IDN_STOP_CANCEL), .d_release = 1 },
    { .kind = EV_CANCEL, .exp_session = X(IDN_SESSION_FREE) },
    END
};

#define M_ROUTE_TRUNC(c)        { .present = true, .ch = (c), .chunk = IDN_CHUNK_VOID, .cfg = true, \
                                  .cfl = ROUTING(0), .sid = 1, .mode = 1, .scwc = 4, .scwc_extra = 1 }

static const step_t s_truncated_config[] = {
    /* From a new link: malformed, nothing allocated */
    RT(.ep = EP_A, .cmd = ACK, .msg = M_ROUTE_TRUNC(0),
       .exp_result = X(IDN_RESULT_INVALID), .exp_session = X(IDN_SESSION_FREE)),
    RT(.now = 0, .ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0), .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    /* From another port: no effect on the live channel */
    RT(.ep = EP_A_PORT2, .cmd = ACK, .msg = M_ROUTE_TRUNC(0),
       .exp_result = X(IDN_RESULT_INVALID), .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE)),
    /* From the connection: the decoder is void until a whole configuration arrives */
    RT(.now = 900000, .ep = EP_A, .cmd = ACK, .msg = M_ROUTE_TRUNC(0),
       .exp_result = X(IDN_RESULT_INVALID), .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED),
       .exp_reason = X(IDN_STOP_ROUTE_CHANGE), .d_release = 1),
    RT(.now = 900000, .ep = EP_A, .cmd = ACK, .msg = M_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_VOIDED), .exp_pev = X(IDN_PEV_CFGERR)),
    RT(.now = 900000, .ep = EP_A, .cmd = ACK, .msg = M_ROUTE_WAVE(0, 0),
       .exp_ch = X(0), .exp_ch_state = X(IDN_CHANNEL_ROUTED_ACTIVE), .exp_pev = X(IDN_PEV_CNLRTED),
       .d_compile = 1, .d_acquire = 1, .d_enqueue = 1),
    /* The truncated packet did not refresh the link; the later valid ones did */
    TICK(.now = 1899999, .exp_session = X(IDN_SESSION_ACTIVE)),
    END
};

static const scenario_t k_scenarios[] = {
    { "empty RT allocates session only",            s_empty_allocates_only },
    { "routing + Void chunk -> ROUTED_IDLE",         s_route_void_idle },
    { "first matching wave chunk acquires output",   s_first_wave_acquires },
    { "data on CLOSED channel: no auto-open, CCErr", s_no_auto_open },
    { "second continuous channel -> BSYErr",         s_second_channel_busy },
    { "unknown service/mode -> SMErr",               s_unknown_service_mode },
    { "SCM != SDM voids decoder, DCMErr",            s_scm_mismatch },
    { "last channel close keeps connection",         s_last_channel_close },
    { "empty close without connection -> 0xEB",      s_empty_close_no_connection },
    { "same IP, other port is a distinct link",      s_same_ip_other_port },
    { "abort cancels output and closes",             s_abort },
    { "one-second link timeout",                     s_link_timeout },
    { "management request while streaming",         s_management_while_streaming },
    { "open + data + close in one message",          s_open_data_close_one_message },
    { "repeated identical configuration",            s_repeated_config },
    { "failed decoder replacement",                  s_failed_replacement },
    { "client group exclusion",                      s_group_exclusion },
    { "repeated close",                              s_repeated_close },
    { "close command carrying a message",            s_close_with_message },
    { "two destination interfaces",                  s_two_interfaces },
    { "malformed packets",                           s_malformed },
    { "release deadline -> FAULTED until quiescent", s_release_deadline_fault },
    { "output held by another producer",             s_output_busy_elsewhere },
    { "Void service mode",                           s_void_mode },
    { "service ID 0 selects default",                s_default_service_id },
    { "chunk type and minimum length",               s_wrong_chunk_type },
    { "local cancel (web Stop)",                     s_user_cancel },
    { "truncated configuration voids the decoder",   s_truncated_config },
};

/* -- Runner ----------------------------------------------------------------- */

static int g_failures;

#define CHECK(cond, ...) do {                                         \
        if (!(cond)) {                                                \
            printf("  FAIL %s step %d: ", name, step_no);             \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
            failed = true;                                            \
        }                                                             \
    } while (0)

static bool run_scenario(const scenario_t *sc)
{
    static idn_session_t s;
    static uint8_t buf[0x10000];
    const char *name = sc->name;
    bool failed = false;

    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.acquire_ok = true;
    g_fake.release_ok = true;
    g_fake.quiescent = true;
    idn_session_init(&s, &k_ops);

    int step_no = 0;
    for (const step_t *st = sc->steps; st->kind != EV_END; st++, step_no++) {
        fake_t before = g_fake;
        idn_rt_outcome_t out;
        uint8_t resp[IDN_GROUP_RESP_BYTES] = { 0 };
        bool have_out = false;

        switch (st->kind) {
            case EV_RT: {
                size_t n = build_msg(&st->msg, buf);
                idn_session_rt(&s, st->now, &k_ep[st->ep], st->cmd, st->group, (uint16_t)step_no,
                               buf, n, &out);
                have_out = true;
                break;
            }
            case EV_RT_RAW:
                idn_session_rt(&s, st->now, &k_ep[st->ep], st->cmd, st->group, (uint16_t)step_no,
                               st->raw, st->raw_len, &out);
                have_out = true;
                break;
            case EV_TICK:
                idn_session_tick(&s, st->now);
                break;
            case EV_GROUP:
                idn_session_group_request(&s, st->raw, st->raw_len, resp);
                CHECK(resp[0] == IDN_GROUP_RESP_BYTES, "group response size %u", resp[0]);
                if (st->exp_result) {
                    CHECK(resp[1] == st->exp_result - 1, "group code 0x%02X, want 0x%02X",
                          resp[1], st->exp_result - 1);
                }
                if (st->exp_mask) {
                    unsigned mask = (unsigned)(resp[2] << 8 | resp[3]);
                    CHECK(mask == (unsigned)(st->exp_mask - 1), "mask 0x%04X, want 0x%04X",
                          mask, st->exp_mask - 1);
                }
                break;
            case EV_CANCEL:
                idn_session_cancel(&s, IDN_STOP_CANCEL);
                break;
            case EV_SET:
                if (st->set_acquire_ok) g_fake.acquire_ok = st->set_acquire_ok - 1;
                if (st->set_release_ok) g_fake.release_ok = st->set_release_ok - 1;
                if (st->set_quiescent)  g_fake.quiescent = st->set_quiescent - 1;
                continue;
            default:
                break;
        }

        if (have_out && st->exp_result) {
            CHECK(out.result == st->exp_result - 1, "result 0x%02X, want 0x%02X",
                  out.result, st->exp_result - 1);
        }
        if (have_out && (st->exp_iev || st->exp_pev || st->exp_status)) {
            CHECK(out.send_ack, "no ACK produced");
        }
        if (have_out && st->exp_iev) {
            CHECK(out.ack.input_events == st->exp_iev - 1, "input events 0x%04X, want 0x%04X",
                  out.ack.input_events, st->exp_iev - 1);
        }
        if (have_out && st->exp_pev) {
            CHECK(out.ack.pipeline_events == st->exp_pev - 1, "pipeline events 0x%04X, want 0x%04X",
                  out.ack.pipeline_events, st->exp_pev - 1);
        }
        if (have_out && st->exp_status) {
            CHECK(out.ack.status == st->exp_status - 1, "status 0x%02X, want 0x%02X",
                  out.ack.status, st->exp_status - 1);
        }
        if (st->exp_session) {
            CHECK((int)s.state == st->exp_session - 1, "session state %d, want %d",
                  (int)s.state, st->exp_session - 1);
        }
        if (st->exp_ch && st->exp_ch_state) {
            int ch = st->exp_ch - 1;
            CHECK((int)s.ch[ch].state == st->exp_ch_state - 1, "channel %d state %d, want %d",
                  ch, (int)s.ch[ch].state, st->exp_ch_state - 1);
        }
        if (st->exp_reason) {
            CHECK((int)g_fake.last_reason == st->exp_reason - 1, "release reason %d, want %d",
                  (int)g_fake.last_reason, st->exp_reason - 1);
        }

        CHECK(g_fake.compiles - before.compiles == st->d_compile, "compiles +%d, want +%d",
              g_fake.compiles - before.compiles, st->d_compile);
        CHECK(g_fake.acquires - before.acquires == st->d_acquire, "acquires +%d, want +%d",
              g_fake.acquires - before.acquires, st->d_acquire);
        CHECK(g_fake.enqueues - before.enqueues == st->d_enqueue, "enqueues +%d, want +%d",
              g_fake.enqueues - before.enqueues, st->d_enqueue);
        CHECK(g_fake.releases - before.releases == st->d_release, "releases +%d, want +%d",
              g_fake.releases - before.releases, st->d_release);
        CHECK(g_fake.violations == 0, "ownership contract violated (%d)", g_fake.violations);

        /* Invariants after every step */
        CHECK(s.output_held == g_fake.held, "session/output hold disagree");
        if (s.state != IDN_SESSION_ACTIVE) {
            CHECK(!s.output_held, "output held without an active connection");
        }
        int routed_laser = 0;
        for (int i = 0; i < IDN_SESSION_CHANNELS; i++) {
            if (s.ch[i].route == IDN_ROUTE_LASER_CONTINUOUS) routed_laser++;
            if (s.ch[i].state == IDN_CHANNEL_ROUTED_ACTIVE) {
                CHECK(s.output_held && s.laser_channel == i, "channel %d active without output", i);
            }
        }
        CHECK(routed_laser <= 1, "%d channels hold the exclusive route", routed_laser);
    }

    return !failed;
}

/* All 64 channel IDs route, play and close through the one physical route. */
static bool test_all_channel_ids(void)
{
    static idn_session_t s;
    static uint8_t buf[2048];
    const char *name = "all 64 channel identifiers";
    int step_no = 0;
    bool failed = false;
    idn_rt_outcome_t out;

    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.acquire_ok = g_fake.release_ok = g_fake.quiescent = true;
    idn_session_init(&s, &k_ops);

    for (int ch = 0; ch < IDN_SESSION_CHANNELS; ch++, step_no++) {
        msg_spec_t open = M_ROUTE_WAVE(0, 0);
        msg_spec_t close = M_CLOSE_CH(0);
        open.ch = (uint8_t)ch;
        close.ch = (uint8_t)ch;

        size_t n = build_msg(&open, buf);
        idn_session_rt(&s, 0, &k_ep[EP_A], ACK, 0, 0, buf, n, &out);
        CHECK(s.ch[ch].state == IDN_CHANNEL_ROUTED_ACTIVE, "channel %d not active", ch);
        CHECK(out.ack.pipeline_events == IDN_PEV_CNLRTED, "channel %d events 0x%04X", ch,
              out.ack.pipeline_events);

        n = build_msg(&close, buf);
        idn_session_rt(&s, 0, &k_ep[EP_A], ACK, 0, 0, buf, n, &out);
        CHECK(s.ch[ch].state == IDN_CHANNEL_CLOSED, "channel %d not closed", ch);
    }

    CHECK(g_fake.acquires == 64 && g_fake.releases == 64 && g_fake.enqueues == 64,
          "acquire/enqueue/release %d/%d/%d", g_fake.acquires, g_fake.enqueues, g_fake.releases);
    CHECK(g_fake.violations == 0, "ownership contract violated");
    CHECK(s.state == IDN_SESSION_ACTIVE, "connection closed");
    return !failed;
}

/* Every output generation is new; no enqueue reuses an old one. */
static bool test_generations_advance(void)
{
    static idn_session_t s;
    static uint8_t buf[2048];
    const char *name = "output generation advances";
    int step_no = 0;
    bool failed = false;
    idn_rt_outcome_t out;
    msg_spec_t open = M_ROUTE_WAVE(0, 0);

    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.acquire_ok = g_fake.release_ok = g_fake.quiescent = true;
    idn_session_init(&s, &k_ops);

    uint32_t last = 0;
    for (int i = 0; i < 5; i++, step_no++) {
        size_t n = build_msg(&open, buf);
        idn_session_rt(&s, 0, &k_ep[EP_A], ACK, 0, 0, buf, n, &out);
        CHECK(g_fake.held_gen > last, "generation %u not after %u", g_fake.held_gen, last);
        last = g_fake.held_gen;
        idn_session_rt(&s, 0, &k_ep[EP_A], ABORT, 0, 0, NULL, 0, &out);
    }
    CHECK(g_fake.violations == 0, "ownership contract violated");
    return !failed;
}

int main(void)
{
    int total = 0;

    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); i++) {
        bool ok = run_scenario(&k_scenarios[i]);
        printf("%s  %s\n", ok ? "ok  " : "FAIL", k_scenarios[i].name);
        g_failures += ok ? 0 : 1;
        total++;
    }

    bool ok = test_all_channel_ids();
    printf("%s  all 64 channel identifiers\n", ok ? "ok  " : "FAIL");
    g_failures += ok ? 0 : 1;
    total++;

    ok = test_generations_advance();
    printf("%s  output generation advances\n", ok ? "ok  " : "FAIL");
    g_failures += ok ? 0 : 1;
    total++;

    printf("\n%d/%d passed\n", total - g_failures, total);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
