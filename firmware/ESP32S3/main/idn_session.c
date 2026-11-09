/*
 * idn_session.c
 *
 * Pure IDN-RT session/channel state machine. See idn_session.h.
 *
 * Order for one RT packet:
 *   1. Bounds-check the message (no state touched on failure).
 *   2. Client group, abort, connection identity / allocation.
 *   3. Sequence hook (plan 03), then the link timeout is refreshed.
 *   4. Routing: resolve service/mode, take the exclusive route, build the
 *      decoder, store SDM. Repeated identical configuration changes nothing.
 *   5. Data: closed channel -> CCErr; SCM != SDM -> void the channel;
 *      checked chunk -> take the output on the first one, then enqueue.
 *   6. Channel Close, then connection close.
 *   7. ACK snapshot: latched events are reported once and cleared.
 */

#include <string.h>

#include "idn_session.h"

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool endpoint_eq(const idn_endpoint_t *a, const idn_endpoint_t *b)
{
    return a->peer_addr == b->peer_addr &&
           a->peer_port == b->peer_port &&
           a->local_addr == b->local_addr;
}

void idn_session_init(idn_session_t *s, const idn_session_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    if (ops) {
        s->ops = *ops;
    }
    s->group_mask = 0xFFFF;     /* Hello 3.2: all groups allowed by default */
    s->state = IDN_SESSION_FREE;
    s->laser_channel = -1;
}

bool idn_session_parse_message(const uint8_t *p, size_t len, idn_msg_t *m)
{
    memset(m, 0, sizeof(*m));

    if (!p || len < 8 || len > 0xFF00) {
        return false;
    }

    uint16_t total = rd16(p);
    if (total < 8 || total != len) {
        return false;
    }

    uint8_t cnl = p[2];
    m->chunk_type = p[3];
    m->timestamp = rd32(p + 4);

    if ((cnl & 0x80) == 0) {
        return true;    /* not a channel message; the session reports MVErr */
    }

    m->channel_msg = true;
    m->channel_id = cnl & 0x3F;

    size_t pos = 8;

    /* CCLF on a sequel fragment marks the last fragment, not a config header. */
    if ((cnl & 0x40) && m->chunk_type < IDN_CHUNK_SEQUEL_MIN) {
        if (len - pos < 4) {
            return false;
        }

        uint8_t scwc = p[pos];
        uint8_t cfl = p[pos + 1];

        m->has_cfg = true;
        m->routing = (cfl & 0x01) != 0;
        m->close = (cfl & 0x02) != 0;

        if (m->routing) {
            m->sdm = (uint8_t)((cfl >> 4) & 0x03);
            m->service_id = p[pos + 2];
            m->service_mode = p[pos + 3];
        }
        pos += 4;

        if (m->routing) {
            size_t cfg_bytes = (size_t)scwc * 4u;
            if (len - pos < cfg_bytes) {
                m->cfg_truncated = true;
                return false;
            }
            m->cfg = p + pos;
            m->cfg_len = (uint16_t)cfg_bytes;
            pos += cfg_bytes;
        } else if (scwc != 0) {
            /*
               Stream 2.2: SCWC is 0 without Routing and is ignored. Skipping
               it or not would each put the chunk somewhere different, so a
               message that sets it is refused rather than guessed at.
            */
            return false;
        }
    }

    m->data = p + pos;
    m->data_len = (uint16_t)(len - pos);

    if (m->chunk_type == IDN_CHUNK_VOID && m->data_len != 0) {
        return false;   /* Void has no data section */
    }

    return true;
}

static bool any_channel_open(const idn_session_t *s)
{
    for (int i = 0; i < IDN_SESSION_CHANNELS; i++) {
        if (s->ch[i].state != IDN_CHANNEL_CLOSED) {
            return true;
        }
    }
    return false;
}

static bool output_fault_cleared(idn_session_t *s)
{
    if (s->output_fault && s->ops.output_quiescent(s->ops.ctx)) {
        s->output_fault = false;
    }
    return !s->output_fault;
}

static void release_output(idn_session_t *s, idn_stop_reason_t reason)
{
    if (!s->output_held) {
        return;
    }

    s->output_held = false;

    if (!s->ops.output_release(s->ops.ctx, s->output_gen, reason)) {
        s->output_fault = true;
        s->fault_count++;
    }

    if (s->laser_channel >= 0 &&
        s->ch[s->laser_channel].state == IDN_CHANNEL_ROUTED_ACTIVE) {
        s->ch[s->laser_channel].state = IDN_CHANNEL_ROUTED_IDLE;
    }
}

/* Stop output for the continuous route and drop its decoder. */
static void drop_laser_route(idn_session_t *s, idn_stop_reason_t reason)
{
    int ch = s->laser_channel;
    if (ch < 0) {
        return;
    }

    release_output(s, reason);

    s->laser_channel = -1;
    s->laser_cfg_len = 0;
    s->ch[ch].decoder_valid = false;

    if (s->ops.config_discard) {
        s->ops.config_discard(s->ops.ctx, (uint8_t)ch);
    }
}

static void void_channel(idn_session_t *s, uint8_t ch, idn_void_reason_t why)
{
    idn_channel_t *c = &s->ch[ch];

    if (s->laser_channel == ch) {
        drop_laser_route(s, IDN_STOP_ROUTE_CHANGE);
    }

    c->state = IDN_CHANNEL_VOIDED;
    c->route = IDN_ROUTE_NONE;
    c->void_reason = why;
    c->decoder_valid = false;
}

static void close_channel(idn_session_t *s, uint8_t ch, idn_stop_reason_t reason)
{
    idn_channel_t *c = &s->ch[ch];

    if (s->laser_channel == ch) {
        drop_laser_route(s, reason);
    }

    c->state = IDN_CHANNEL_CLOSED;
    c->route = IDN_ROUTE_NONE;
    c->void_reason = IDN_VOID_NONE;
    c->decoder_valid = false;
    c->sdm = 0;
}

static void close_connection(idn_session_t *s, idn_stop_reason_t reason)
{
    if (s->state != IDN_SESSION_ACTIVE) {
        return;
    }

    s->state = IDN_SESSION_CLOSING;

    for (int i = 0; i < IDN_SESSION_CHANNELS; i++) {
        if (s->ch[i].state != IDN_CHANNEL_CLOSED) {
            close_channel(s, (uint8_t)i, reason);
        }
        s->ch[i].pipeline_events = 0;
    }

    memset(&s->peer, 0, sizeof(s->peer));
    s->groups_seen = 0;
    s->input_events = 0;

    /* A producer that missed its stop deadline keeps the session unavailable. */
    s->state = s->output_fault ? IDN_SESSION_FAULTED : IDN_SESSION_FREE;
}

static void open_connection(idn_session_t *s, const idn_endpoint_t *from, int64_t now_us)
{
    s->state = IDN_SESSION_ACTIVE;
    s->peer = *from;
    s->groups_seen = 0;
    s->last_rt_us = now_us;
    s->conn_gen++;
    s->input_events = IDN_IEV_NEW;
}

static void route_channel(idn_session_t *s, const idn_msg_t *m)
{
    uint8_t ch = m->channel_id;
    idn_channel_t *c = &s->ch[ch];

    /* Service ID 0 selects the default service for the mode; ours is ID 1. */
    bool service_ok = m->service_id == 0 || m->service_id == IDN_SESSION_LASER_SERVICE;
    idn_route_t target = IDN_ROUTE_NONE;

    if (service_ok && m->service_mode == IDN_MODE_VOID) {
        target = IDN_ROUTE_VOID_MODE;
    } else if (service_ok && m->service_mode == IDN_MODE_GRAPHIC_CONTINUOUS) {
        target = IDN_ROUTE_LASER_CONTINUOUS;
    }

    if (target == IDN_ROUTE_NONE) {
        /* Unknown service, or a mode this service does not implement. */
        void_channel(s, ch, IDN_VOID_INVALID_SERVICE);
        c->route_gen++;
        c->pipeline_events |= IDN_PEV_SMERR | IDN_PEV_CNLRTED;
        return;
    }

    if (target == IDN_ROUTE_VOID_MODE) {
        if (c->route == IDN_ROUTE_VOID_MODE) {
            return;
        }
        if (s->laser_channel == ch) {
            drop_laser_route(s, IDN_STOP_ROUTE_CHANGE);
        }
        c->state = IDN_CHANNEL_ROUTED_IDLE;
        c->route = IDN_ROUTE_VOID_MODE;
        c->void_reason = IDN_VOID_NONE;
        c->decoder_valid = false;
        c->sdm = m->sdm;
        c->route_gen++;
        c->pipeline_events |= IDN_PEV_CNLRTED;
        return;
    }

    /* Continuous graphics is exclusive per service (Stream 3.2). */
    if (s->laser_channel >= 0 && s->laser_channel != ch) {
        void_channel(s, ch, IDN_VOID_BUSY);
        c->route_gen++;
        c->pipeline_events |= IDN_PEV_BSYERR | IDN_PEV_CNLRTED;
        return;
    }

    bool same_route = s->laser_channel == ch;

    if (same_route && c->decoder_valid && c->sdm == m->sdm &&
        s->laser_cfg_len == m->cfg_len &&
        (m->cfg_len == 0 || memcmp(s->laser_cfg, m->cfg, m->cfg_len) == 0)) {
        return;     /* repeated configuration: not a change */
    }

    idn_cfg_result_t r = s->ops.config_compile(s->ops.ctx, ch, m->service_mode,
                                               m->cfg, m->cfg_len);
    if (r != IDN_CFG_OK) {
        /* The old decoder must not read data meant for the new configuration. */
        void_channel(s, ch, IDN_VOID_CONFIG);
        if (!same_route && s->ops.config_discard) {
            s->ops.config_discard(s->ops.ctx, ch);
        }
        if (!same_route) {
            c->route_gen++;
            c->pipeline_events |= IDN_PEV_CNLRTED;
        }
        c->pipeline_events |= (r == IDN_CFG_NO_RESOURCES) ? IDN_PEV_IAPERR : IDN_PEV_CFGERR;
        return;
    }

    s->laser_channel = ch;
    s->laser_cfg_len = m->cfg_len;
    if (m->cfg_len) {
        memcpy(s->laser_cfg, m->cfg, m->cfg_len);
    }

    c->route = IDN_ROUTE_LASER_CONTINUOUS;
    c->void_reason = IDN_VOID_NONE;
    c->decoder_valid = true;
    c->sdm = m->sdm;
    c->config_gen++;

    if (!same_route) {
        c->state = IDN_CHANNEL_ROUTED_IDLE;
        c->route_gen++;
        c->pipeline_events |= IDN_PEV_CNLRTED;
    }
}

static uint16_t chunk_result_event(idn_chunk_result_t r)
{
    switch (r) {
        case IDN_CHUNK_OK:              return 0;
        case IDN_CHUNK_TOO_SHORT:       return IDN_PEV_MCLERR;
        case IDN_CHUNK_PAYLOAD_INVALID: return IDN_PEV_PVLERR;
        case IDN_CHUNK_UNSUPPORTED:     return IDN_PEV_RGUERR;
        case IDN_CHUNK_DEVICE_OVERRUN:  return IDN_PEV_DVIERR;
        default:                        return IDN_PEV_IAPERR;
    }
}

static void process_data(idn_session_t *s, const idn_msg_t *m)
{
    uint8_t ch = m->channel_id;
    idn_channel_t *c = &s->ch[ch];

    if (m->chunk_type == IDN_CHUNK_VOID) {
        return;
    }

    if (c->state == IDN_CHANNEL_CLOSED) {
        s->input_events |= IDN_IEV_CCERR;   /* never auto-open */
        return;
    }

    if (c->state == IDN_CHANNEL_VOIDED) {
        if (c->void_reason == IDN_VOID_CONFIG || c->void_reason == IDN_VOID_SCM_MISMATCH) {
            c->pipeline_events |= IDN_PEV_CFGERR;   /* data with no decoder */
        }
        return;
    }

    if (c->route == IDN_ROUTE_VOID_MODE) {
        return;     /* Stream 2.2: Void mode ignores data chunks */
    }

    if (m->chunk_type != IDN_CHUNK_WAVE) {
        c->pipeline_events |= IDN_PEV_CKTERR;   /* continuous mode takes wave samples only */
        return;
    }

    if (m->data_len < 4) {
        c->pipeline_events |= IDN_PEV_MCLERR;
        return;
    }

    uint8_t scm = (uint8_t)((m->data[0] >> 4) & 0x03);
    if (scm != c->sdm) {
        /* Stream 6.7: discard the configuration, void, wait for a new one. */
        void_channel(s, ch, IDN_VOID_SCM_MISMATCH);
        c->pipeline_events |= IDN_PEV_DCMERR;
        return;
    }

    if (!c->decoder_valid) {
        c->pipeline_events |= IDN_PEV_CFGERR;
        return;
    }

    idn_chunk_result_t r = s->ops.wave_check(s->ops.ctx, ch, m->data, m->data_len);
    if (r != IDN_CHUNK_OK) {
        c->pipeline_events |= chunk_result_event(r);
        return;
    }

    if (s->ops.wave_timeline) {
        uint16_t ev = 0;
        bool keep = s->ops.wave_timeline(s->ops.ctx, ch, c->route_gen, m->timestamp,
                                         m->data, m->data_len, &ev);
        c->pipeline_events |= ev;
        if (!keep) {
            return;
        }
    }

    if (!s->output_held) {
        if (!output_fault_cleared(s)) {
            c->pipeline_events |= IDN_PEV_DVIERR;
            return;
        }

        uint32_t gen = s->output_gen + 1;
        if (!s->ops.output_acquire(s->ops.ctx, gen)) {
            c->pipeline_events |= IDN_PEV_BSYERR;   /* another producer owns the laser */
            return;
        }
        s->output_gen = gen;
        s->output_held = true;
    }

    c->state = IDN_CHANNEL_ROUTED_ACTIVE;

    r = s->ops.wave_enqueue(s->ops.ctx, ch, s->output_gen, m->data, m->data_len);
    c->pipeline_events |= chunk_result_event(r);
}

/* Returns the message's channel, or -1 when it is not a channel message. */
static int process_message(idn_session_t *s, const idn_msg_t *m)
{
    if (!m->channel_msg) {
        s->input_events |= IDN_IEV_MVERR;
        return -1;
    }

    uint8_t ch = m->channel_id;

    if (m->has_cfg && m->routing) {
        route_channel(s, m);
    }

    if (m->chunk_type >= IDN_CHUNK_SEQUEL_MIN) {
        idn_channel_t *c = &s->ch[ch];
        if (c->state == IDN_CHANNEL_CLOSED) {
            s->input_events |= IDN_IEV_CCERR;
        } else if (c->route == IDN_ROUTE_LASER_CONTINUOUS) {
            c->pipeline_events |= IDN_PEV_CKTERR;
        }
    } else {
        process_data(s, m);
    }

    /* Close follows this message's data; the connection stays open. */
    if (m->has_cfg && m->close && s->ch[ch].state != IDN_CHANNEL_CLOSED) {
        close_channel(s, ch, IDN_STOP_CHANNEL_CLOSE);
    }

    return ch;
}

static void plain_result(idn_rt_outcome_t *out, uint8_t result)
{
    out->result = result;
    out->ack.result = result;
}

void idn_session_rt(idn_session_t *s, int64_t now_us, const idn_endpoint_t *from,
                    uint8_t cmd, uint8_t flags, uint16_t seq,
                    const uint8_t *payload, size_t payload_len,
                    idn_rt_outcome_t *out)
{
    memset(out, 0, sizeof(*out));

    bool ack_req  = cmd == IDN_CMD_RT_CHANNEL_ACK || cmd == IDN_CMD_RT_CLOSE_ACK;
    bool is_close = cmd == IDN_CMD_RT_CLOSE || cmd == IDN_CMD_RT_CLOSE_ACK;
    bool is_abort = cmd == IDN_CMD_RT_ABORT;
    uint8_t group = flags & 0x0F;

    if (!ack_req && !is_close && !is_abort && cmd != IDN_CMD_RT_CHANNEL) {
        plain_result(out, IDN_RESULT_INVALID);
        return;
    }

    out->send_ack = ack_req;

    /* Abort carries no payload; it is acted on whatever follows the header. */
    idn_msg_t msg;
    bool has_msg = !is_abort && payload_len > 0;
    if (has_msg && !idn_session_parse_message(payload, payload_len, &msg)) {
        /*
           A configuration cut short on the live connection still means its
           sender changed or lost the channel's configuration: nothing on that
           channel decodes until a whole one arrives. The packet stays
           malformed: no link refresh, no other processing.
        */
        if (msg.cfg_truncated && s->state == IDN_SESSION_ACTIVE &&
            endpoint_eq(&s->peer, from) && !idn_session_group_excluded(s, group) &&
            s->ch[msg.channel_id].state != IDN_CHANNEL_CLOSED) {
            void_channel(s, msg.channel_id, IDN_VOID_CONFIG);
            s->ch[msg.channel_id].pipeline_events |= IDN_PEV_CFGERR;
        }
        plain_result(out, IDN_RESULT_INVALID);
        return;
    }

    if (idn_session_group_excluded(s, group)) {
        plain_result(out, IDN_RESULT_EXCLUDED);
        return;
    }

    bool match = s->state == IDN_SESSION_ACTIVE && endpoint_eq(&s->peer, from);

    if (is_abort) {
        if (match) {
            close_connection(s, IDN_STOP_ABORT);
            out->connection_closed = true;
            out->accepted = true;
        }
        plain_result(out, IDN_RESULT_OK);
        return;
    }

    if (!match) {
        if (is_close && !has_msg) {
            plain_result(out, IDN_RESULT_EMPTY_CLOSE);
            return;
        }
        if (s->state != IDN_SESSION_FREE) {
            plain_result(out, IDN_RESULT_OCCUPIED);
            return;
        }
        open_connection(s, from, now_us);
        out->connection_opened = true;
    }

    if (s->ops.rt_input) {
        uint16_t ev = 0;
        bool keep = s->ops.rt_input(s->ops.ctx, s->conn_gen, seq, out->connection_opened, &ev);
        s->input_events |= ev;
        if (!keep) {
            plain_result(out, IDN_RESULT_OK);
            if (ack_req) {
                out->ack.input_events = s->input_events;
                s->input_events = 0;
                out->ack.status = any_channel_open(s) ? IDN_STATUS_SOCNL : 0;
            }
            return;
        }
    }

    s->groups_seen |= (uint16_t)(1u << group);
    s->last_rt_us = now_us;
    out->accepted = true;

    int ack_ch = has_msg ? process_message(s, &msg) : -1;

    /* Snapshot before a connection close resets the latches. */
    uint16_t pev = 0;
    uint16_t iev = 0;
    if (ack_req) {
        if (ack_ch >= 0) {
            pev = s->ch[ack_ch].pipeline_events;
            s->ch[ack_ch].pipeline_events = 0;
        }
        iev = s->input_events;
        s->input_events = 0;
    }

    if (is_close) {
        close_connection(s, IDN_STOP_GRACEFUL);
        out->connection_closed = true;
    }

    plain_result(out, IDN_RESULT_OK);

    if (ack_req) {
        if (ack_ch >= 0 && s->ch[ack_ch].state == IDN_CHANNEL_CLOSED) {
            pev |= IDN_PEV_CNLCLSD;
        }
        out->ack.input_events = iev;
        out->ack.pipeline_events = pev;
        out->ack.status = (s->state == IDN_SESSION_ACTIVE && any_channel_open(s))
                              ? IDN_STATUS_SOCNL : 0;
    }
}

void idn_session_tick(idn_session_t *s, int64_t now_us)
{
    if (s->state == IDN_SESSION_ACTIVE &&
        now_us - s->last_rt_us >= IDN_SESSION_LINK_TIMEOUT_US) {
        close_connection(s, IDN_STOP_TIMEOUT);
    }

    if (s->output_fault && output_fault_cleared(s) && s->state == IDN_SESSION_FAULTED) {
        s->state = IDN_SESSION_FREE;
    }
}

void idn_session_cancel(idn_session_t *s, idn_stop_reason_t reason)
{
    close_connection(s, reason);
}

void idn_session_group_request(idn_session_t *s, const uint8_t *payload,
                               size_t payload_len,
                               uint8_t resp[IDN_GROUP_RESP_BYTES])
{
    uint8_t code;

    if (!payload || payload_len < IDN_GROUP_REQ_BYTES ||
        payload[0] < IDN_GROUP_REQ_BYTES || payload[0] > payload_len) {
        code = IDN_GROUP_INVALID;
    } else if (payload[1] == 0x01) {
        code = IDN_GROUP_OK;
    } else if (payload[1] == 0x02) {
        if (memcmp(payload + 4, s->group_auth, IDN_SESSION_AUTH_BYTES) != 0) {
            code = IDN_GROUP_AUTH_ERROR;
        } else {
            s->group_mask = rd16(payload + 2);
            code = IDN_GROUP_OK;

            /* Hello 3.2.1: links of excluded groups are closed. */
            if (s->state == IDN_SESSION_ACTIVE && (s->groups_seen & (uint16_t)~s->group_mask)) {
                close_connection(s, IDN_STOP_EXCLUDED);
            }
        }
    } else {
        code = IDN_GROUP_UNSUPPORTED;
    }

    resp[0] = IDN_GROUP_RESP_BYTES;
    resp[1] = code;
    resp[2] = (uint8_t)(s->group_mask >> 8);
    resp[3] = (uint8_t)(s->group_mask & 0xFF);
}

bool idn_session_set_group_auth(idn_session_t *s, const uint8_t *code, size_t len)
{
    if (len > IDN_SESSION_AUTH_BYTES || (len && !code)) {
        return false;
    }
    memset(s->group_auth, 0, sizeof(s->group_auth));
    if (len) {
        memcpy(s->group_auth, code, len);
    }
    return true;
}

bool idn_session_occupied(const idn_session_t *s)
{
    return s->state != IDN_SESSION_FREE;
}

bool idn_session_group_excluded(const idn_session_t *s, uint8_t group)
{
    return (s->group_mask & (1u << (group & 0x0F))) == 0;
}
