/*
 * idn_timing.c
 *
 * Pure-C IDN timing and diagnostic helpers. See idn_timing.h for the
 * policies; this file only implements them.
 */

#include "idn_timing.h"

#include <string.h>

#define US_PER_S            1000000ull

/*
   Bounded estimation window for the rate envelope: once an epoch's summed
   duration passes about 16.8 s, samples and duration are halved together,
   which keeps the ratio and lets the estimate follow a long stream.
*/
#define IDN_RATE_WINDOW_US  (1ull << 24)

/* -- Sequence -------------------------------------------------------------- */

static void seq_baseline(idn_seq_tracker_t *t, uint16_t seq)
{
    t->valid      = true;
    t->highest    = seq;
    t->seen       = 1u;
    t->span       = 1;
    t->stale_run  = 0;
    t->stale_next = 0;
}

/* Move the window forward by d (1..0x7FFF); returns numbers that left it unseen. */
static uint32_t seq_advance(idn_seq_tracker_t *t, uint32_t d)
{
    uint32_t missing = 0;

    for (uint32_t k = 0; k < t->span; k++) {
        if (k + d >= IDN_SEQ_WINDOW && !(t->seen & (1u << k))) {
            missing++;
        }
    }

    /* Skipped numbers land at offsets 1..d-1; those at offset >= window leave at once. */
    if (d > IDN_SEQ_WINDOW) {
        missing += d - IDN_SEQ_WINDOW;
    }

    t->seen = (d >= IDN_SEQ_WINDOW) ? 0u : (t->seen << d);
    t->seen |= 1u;

    uint32_t span = (uint32_t)t->span + d;
    t->span = (uint8_t)(span > IDN_SEQ_WINDOW ? IDN_SEQ_WINDOW : span);
    t->highest = (uint16_t)(t->highest + d);

    return missing;
}

void idn_seq_reset(idn_seq_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
}

idn_seq_result_t idn_seq_observe(idn_seq_tracker_t *t, uint16_t seq)
{
    idn_seq_result_t r = { IDN_SEQ_FIRST, true, 0, 0 };

    if (!t->valid) {
        seq_baseline(t, seq);
        return r;
    }

    uint16_t fwd = (uint16_t)(seq - t->highest);
    if (fwd != 0 && fwd < 0x8000u) {
        t->stale_run = 0;
        r.cls = (fwd == 1) ? IDN_SEQ_NEXT : IDN_SEQ_GAP;
        r.missing = seq_advance(t, fwd);
        if (fwd != 1) {
            r.input_events |= IDN_INEV_SEQ_NOT_NEXT;
        }
        if (r.missing) {
            r.input_events |= IDN_INEV_SEQ_NOT_NEXT | IDN_INEV_SEQ_MISSING;
        }
        return r;
    }

    r.accept = false;
    r.input_events = IDN_INEV_SEQ_NOT_NEXT;

    uint16_t back = (uint16_t)(t->highest - seq);
    if (back < t->span) {
        uint32_t bit = 1u << back;
        t->stale_run = 0;
        if (t->seen & bit) {
            r.cls = IDN_SEQ_DUPLICATE;
            r.input_events |= IDN_INEV_SEQ_DUPLICATE;
        } else {
            t->seen |= bit;
            r.cls = IDN_SEQ_LATE;
        }
        return r;
    }

    if (t->stale_run > 0 && seq == t->stale_next) {
        t->stale_run++;
    } else {
        t->stale_run = 1;
    }
    t->stale_next = (uint16_t)(seq + 1u);

    if (t->stale_run >= IDN_SEQ_RESYNC_RUN) {
        seq_baseline(t, seq);
        r.cls = IDN_SEQ_RESYNC;
        r.accept = true;
        return r;
    }

    r.cls = IDN_SEQ_STALE;
    return r;
}

/* -- Chronology ------------------------------------------------------------ */

bool idn_chunk_has_timestamp(uint8_t chunk_type)
{
    return chunk_type < 0xC0u;
}

uint64_t idn_ts_unwrap(uint64_t reference_us, uint32_t wire_us)
{
    int32_t diff = (int32_t)(wire_us - (uint32_t)reference_us);
    return reference_us + (uint64_t)(int64_t)diff;
}

void idn_chronology_reset(idn_chronology_t *c)
{
    memset(c, 0, sizeof(*c));
}

idn_ts_order_t idn_chronology_check(const idn_chronology_t *c, uint32_t wire_us,
                                    uint64_t *unwrapped_us)
{
    uint64_t ts;

    if (!c->valid) {
        /* Start one wrap in, so an earlier timestamp never goes below zero. */
        ts = (1ull << 32) | wire_us;
        if (unwrapped_us) *unwrapped_us = ts;
        return IDN_TS_FIRST;
    }

    ts = idn_ts_unwrap(c->last_us, wire_us);
    if (unwrapped_us) *unwrapped_us = ts;

    if (ts == c->last_us) return IDN_TS_EQUAL;
    return (ts > c->last_us) ? IDN_TS_FORWARD : IDN_TS_BEFORE;
}

void idn_chronology_commit(idn_chronology_t *c, uint64_t unwrapped_us)
{
    if (!c->valid || unwrapped_us > c->last_us) {
        c->last_us = unwrapped_us;
    }
    c->valid = true;
    c->before_run = 0;
}

bool idn_chronology_before(idn_chronology_t *c, uint32_t wire_us, uint64_t *unwrapped_us)
{
    if (c->before_run > 0 && (int32_t)(wire_us - c->before_last) >= 0) {
        c->before_run++;
    } else {
        c->before_run = 1;
    }
    c->before_last = wire_us;

    if (c->before_run < IDN_TS_RESYNC_RUN) {
        return false;
    }

    c->valid      = true;
    c->before_run = 0;
    c->last_us    = (1ull << 32) | wire_us;
    if (unwrapped_us) *unwrapped_us = c->last_us;
    return true;
}

/* -- Wave continuity ------------------------------------------------------- */

void idn_wave_reset(idn_wave_timeline_t *w)
{
    memset(w, 0, sizeof(*w));
}

idn_wave_verdict_t idn_wave_check(const idn_wave_timeline_t *w, uint64_t ts_us)
{
    switch (w->state) {
    case IDN_WAVE_IDLE:
        return w->had_epoch ? IDN_WAVE_RESTART : IDN_WAVE_START;
    case IDN_WAVE_BROKEN:
        return IDN_WAVE_AWAIT_RECOVERY;
    case IDN_WAVE_CONTINUOUS:
    default:
        if (ts_us == w->next_us) return IDN_WAVE_CONTINUES;
        return (ts_us > w->next_us) ? IDN_WAVE_GAP : IDN_WAVE_OVERLAP;
    }
}

void idn_wave_commit(idn_wave_timeline_t *w, uint64_t ts_us, uint32_t duration_us)
{
    w->state   = IDN_WAVE_CONTINUOUS;
    w->next_us = ts_us + duration_us;
}

void idn_wave_break(idn_wave_timeline_t *w)
{
    w->state     = IDN_WAVE_BROKEN;
    w->had_epoch = true;
}

void idn_wave_recover(idn_wave_timeline_t *w)
{
    if (w->state == IDN_WAVE_BROKEN) {
        w->state = IDN_WAVE_IDLE;
    }
}

void idn_wave_end(idn_wave_timeline_t *w)
{
    if (w->state != IDN_WAVE_IDLE) {
        w->had_epoch = true;
    }
    w->state = IDN_WAVE_IDLE;
}

bool idn_wave_verdict_is_error(idn_wave_verdict_t v)
{
    return v == IDN_WAVE_GAP || v == IDN_WAVE_OVERLAP || v == IDN_WAVE_AWAIT_RECOVERY;
}

/* -- Rate policy ----------------------------------------------------------- */

/* Full 128-bit product of two 64-bit values, without compiler extensions. */
static void mul_u64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a_lo = (uint32_t)a, a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b, b_hi = b >> 32;

    uint64_t ll = a_lo * b_lo;
    uint64_t lh = a_lo * b_hi;
    uint64_t hl = a_hi * b_lo;
    uint64_t hh = a_hi * b_hi;

    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;

    *lo = (mid << 32) | (uint32_t)ll;
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* a * b < c * d, exactly. */
static bool prod_less(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t x_hi, x_lo, y_hi, y_lo;
    mul_u64(a, b, &x_hi, &x_lo);
    mul_u64(c, d, &y_hi, &y_lo);
    return x_hi < y_hi || (x_hi == y_hi && x_lo < y_lo);
}

void idn_rate_reset(idn_rate_epoch_t *e)
{
    memset(e, 0, sizeof(*e));
}

idn_rate_verdict_t idn_rate_check(const idn_rate_policy_t *p, const idn_rate_epoch_t *e,
                                  uint32_t samples, uint32_t duration_us,
                                  uint32_t locked_hz)
{
    if (samples == 0 || duration_us == 0) {
        return IDN_RATE_INVALID;
    }

    uint64_t q = p->resolution_us;
    uint64_t n = samples;
    uint64_t d = duration_us;

    /* Upper bound n * 1e6 / (d - q) below the minimum; unbounded when d <= q. */
    if (d > q && prod_less(n, US_PER_S, p->min_hz, d - q)) {
        return IDN_RATE_BELOW_MIN;
    }

    /* Lower bound of the epoch including this chunk above the limit. */
    uint64_t total_n = e->samples + n;
    uint64_t total_d = e->duration_us + d;
    if (prod_less(p->limit_hz, total_d + q, total_n, US_PER_S)) {
        return IDN_RATE_ABOVE_LIMIT;
    }

    if (e->samples > 0) {
        uint64_t en = e->samples;
        uint64_t ed = e->duration_us;

        /* Chunk upper bound n/(d-q) below epoch lower bound en/(ed+q). */
        if (d > q && prod_less(n, ed + q, en, d - q)) {
            return IDN_RATE_CHANGED;
        }
        /* Epoch upper bound en/(ed-q) below chunk lower bound n/(d+q). */
        if (ed > q && prod_less(en, d + q, n, ed - q)) {
            return IDN_RATE_CHANGED;
        }
    }

    if (locked_hz != 0) {
        /* Chunk envelope must reach within 1 Hz of the locked output rate. */
        if (prod_less((uint64_t)locked_hz + 1u, d + q, n, US_PER_S)) {
            return IDN_RATE_CHANGED;
        }
        if (d > q && prod_less(n, US_PER_S, (uint64_t)locked_hz - 1u, d - q)) {
            return IDN_RATE_CHANGED;
        }
    }

    return IDN_RATE_OK;
}

void idn_rate_commit(idn_rate_epoch_t *e, uint32_t samples, uint32_t duration_us)
{
    e->samples     += samples;
    e->duration_us += duration_us;

    while (e->duration_us > IDN_RATE_WINDOW_US) {
        e->samples     /= 2u;
        e->duration_us /= 2u;
    }
}

uint32_t idn_rate_select(const idn_rate_policy_t *p, const idn_rate_epoch_t *e)
{
    if (e->samples == 0 || e->duration_us == 0) {
        return 0;
    }

    uint64_t hz = (e->samples * US_PER_S + e->duration_us / 2u) / e->duration_us;
    if (hz > p->limit_hz) hz = p->limit_hz;
    if (hz < p->min_hz)   hz = p->min_hz;
    return (uint32_t)hz;
}

uint64_t idn_rate_requested_mhz(const idn_rate_epoch_t *e)
{
    if (e->samples == 0 || e->duration_us == 0) {
        return 0;
    }
    return e->samples * US_PER_S * 1000u / e->duration_us;
}

/* -- Event latch ----------------------------------------------------------- */

void idn_event_latch_reset(idn_event_latch_t *l)
{
    memset(l, 0, sizeof(*l));
}

void idn_event_latch_record(idn_event_latch_t *l, uint16_t bits)
{
    l->flags          |= bits;
    l->since_snapshot |= bits;
}

idn_event_report_t idn_event_latch_snapshot(idn_event_latch_t *l)
{
    idn_event_report_t r;

    l->generation++;
    l->since_snapshot = 0;
    r.flags      = l->flags;
    r.generation = l->generation;
    return r;
}

void idn_event_latch_reported(idn_event_latch_t *l, const idn_event_report_t *r)
{
    if (r->generation != l->generation) {
        return;
    }
    l->flags &= (uint16_t)~(r->flags & (uint16_t)~l->since_snapshot);
}

/* -- Serialization --------------------------------------------------------- */

size_t idn_ack_serialize(const idn_ack_fields_t *f, uint8_t out[IDN_ACK_STRUCT_SIZE])
{
    out[0]  = (uint8_t)IDN_ACK_STRUCT_SIZE;
    out[1]  = f->result_code;
    out[2]  = (uint8_t)(f->input_event_flags >> 8);
    out[3]  = (uint8_t)(f->input_event_flags);
    out[4]  = (uint8_t)(f->pipeline_event_flags >> 8);
    out[5]  = (uint8_t)(f->pipeline_event_flags);
    out[6]  = f->status_flags;
    out[7]  = f->link_quality;
    out[8]  = (uint8_t)(f->latency_us >> 24);
    out[9]  = (uint8_t)(f->latency_us >> 16);
    out[10] = (uint8_t)(f->latency_us >> 8);
    out[11] = (uint8_t)(f->latency_us);
    return IDN_ACK_STRUCT_SIZE;
}

uint16_t idn_ack_pipeline_flags(uint16_t latched, bool has_channel, bool channel_closed)
{
    if (!has_channel) {
        return 0;
    }
    uint16_t flags = (uint16_t)(latched & (uint16_t)~IDN_PLEV_CNLCLSD);
    if (channel_closed) {
        flags |= IDN_PLEV_CNLCLSD;
    }
    return flags;
}

uint8_t idn_ack_status_flags(bool open_channels, bool session_has_messages,
                             bool device_may_hold_data)
{
    uint8_t flags = 0;
    if (open_channels)        flags |= IDN_STATUS_SOCNL;
    if (session_has_messages) flags |= IDN_STATUS_SNMSG;
    if (device_may_hold_data) flags |= IDN_STATUS_DOBUF;
    return flags;
}

uint8_t idn_scan_status(const idn_scan_state_t *s)
{
    uint8_t flags = 0;
    if (s->rt_supported)                          flags |= IDN_SCAN_STATUS_RT;
    if (s->sessions_in_use >= s->sessions_total)  flags |= IDN_SCAN_STATUS_OCPD;
    if (s->group_excluded)                        flags |= IDN_SCAN_STATUS_XCLD;
    if (s->offline)                               flags |= IDN_SCAN_STATUS_OFFLINE;
    if (s->malfunction)                           flags |= IDN_SCAN_STATUS_MALFN;
    return flags;
}
