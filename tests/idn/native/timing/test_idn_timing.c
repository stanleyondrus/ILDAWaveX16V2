/*
 * Host-native tests for firmware/ESP32S3/main/idn_timing.c.
 *
 * Expected values are literals worked out from the IDN-Hello draft
 * (2022-03-27) section 6 / 6.3.3 and IDN-Stream rev002 sections 2.1 / 6.1,
 * not recomputed with the module's own algorithms.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "idn_timing.h"

static int s_failures = 0;
static int s_checks   = 0;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { \
        s_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    s_checks++; \
    if (_a != _b) { \
        s_failures++; \
        printf("FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
    } \
} while (0)

#define SEQ_T1  IDN_INEV_SEQ_NOT_NEXT
#define SEQ_T2  IDN_INEV_SEQ_DUPLICATE
#define SEQ_T3  IDN_INEV_SEQ_MISSING

/* -- Sequence -------------------------------------------------------------- */

typedef struct {
    uint16_t        seq;
    idn_seq_class_t cls;
    bool            accept;
    uint16_t        events;
    uint32_t        missing;
} seq_step_t;

static void run_seq_case(const char *name, const seq_step_t *steps, size_t n)
{
    idn_seq_tracker_t t;
    idn_seq_reset(&t);

    for (size_t i = 0; i < n; i++) {
        idn_seq_result_t r = idn_seq_observe(&t, steps[i].seq);
        int before = s_failures;
        CHECK_EQ(r.cls, steps[i].cls);
        CHECK_EQ(r.accept, steps[i].accept);
        CHECK_EQ(r.input_events, steps[i].events);
        CHECK_EQ(r.missing, steps[i].missing);
        if (s_failures != before) {
            printf("  in case '%s' step %u (seq %u)\n", name, (unsigned)i, steps[i].seq);
        }
    }
}

/* Feed seq_from..seq_to (inclusive, mod 2^16) and return the summed missing count. */
static uint32_t seq_feed_range(idn_seq_tracker_t *t, uint16_t from, uint32_t count)
{
    uint32_t missing = 0;
    for (uint32_t i = 0; i < count; i++) {
        idn_seq_result_t r = idn_seq_observe(t, (uint16_t)(from + i));
        missing += r.missing;
    }
    return missing;
}

static void test_seq(void)
{
    static const seq_step_t wrap[] = {
        { 65534, IDN_SEQ_FIRST, true, 0, 0 },
        { 65535, IDN_SEQ_NEXT,  true, 0, 0 },
        { 0,     IDN_SEQ_NEXT,  true, 0, 0 },
        { 1,     IDN_SEQ_NEXT,  true, 0, 0 },
    };
    run_seq_case("wrap 65535 -> 0", wrap, sizeof(wrap) / sizeof(wrap[0]));

    static const seq_step_t dup[] = {
        { 10, IDN_SEQ_FIRST,     true,  0,               0 },
        { 11, IDN_SEQ_NEXT,      true,  0,               0 },
        { 11, IDN_SEQ_DUPLICATE, false, SEQ_T1 | SEQ_T2, 0 },
        { 12, IDN_SEQ_NEXT,      true,  0,               0 },
        { 10, IDN_SEQ_DUPLICATE, false, SEQ_T1 | SEQ_T2, 0 },
    };
    run_seq_case("duplicate", dup, sizeof(dup) / sizeof(dup[0]));

    static const seq_step_t gap_late[] = {
        { 100, IDN_SEQ_FIRST,     true,  0,               0 },
        { 101, IDN_SEQ_NEXT,      true,  0,               0 },
        { 105, IDN_SEQ_GAP,       true,  SEQ_T1,          0 },
        { 103, IDN_SEQ_LATE,      false, SEQ_T1,          0 },
        { 103, IDN_SEQ_DUPLICATE, false, SEQ_T1 | SEQ_T2, 0 },
        { 106, IDN_SEQ_NEXT,      true,  0,               0 },
    };
    run_seq_case("gap then delayed old packet", gap_late, sizeof(gap_late) / sizeof(gap_late[0]));

    /* The holes 102 and 104 are lost; 103 arrived late and is not counted. */
    {
        idn_seq_tracker_t t;
        idn_seq_reset(&t);
        uint32_t missing = 0;
        missing += idn_seq_observe(&t, 100).missing;
        missing += idn_seq_observe(&t, 101).missing;
        missing += idn_seq_observe(&t, 105).missing;
        missing += idn_seq_observe(&t, 103).missing;
        missing += seq_feed_range(&t, 106, 40);
        CHECK_EQ(missing, 2);
    }

    /* A gap across the wrap is a gap of 7, never ~65,535. */
    {
        idn_seq_tracker_t t;
        idn_seq_reset(&t);
        idn_seq_observe(&t, 65530);
        idn_seq_result_t r = idn_seq_observe(&t, 2);
        CHECK_EQ(r.cls, IDN_SEQ_GAP);
        CHECK_EQ(r.missing, 0);
        uint32_t missing = seq_feed_range(&t, 3, 40);
        CHECK_EQ(missing, 7);
    }

    /* Big forward jump: 999 skipped numbers, counted once in total. */
    {
        idn_seq_tracker_t t;
        idn_seq_reset(&t);
        idn_seq_observe(&t, 0);
        idn_seq_result_t r = idn_seq_observe(&t, 1000);
        CHECK_EQ(r.cls, IDN_SEQ_GAP);
        CHECK_EQ(r.missing, 968);
        CHECK_EQ(r.input_events, SEQ_T1 | SEQ_T3);
        uint32_t missing = r.missing + seq_feed_range(&t, 1001, 40);
        CHECK_EQ(missing, 999);
    }

    static const seq_step_t half_range[] = {
        { 0,      IDN_SEQ_FIRST, true,  0,      0 },
        { 0x8000, IDN_SEQ_STALE, false, SEQ_T1, 0 },
        { 0x7FFF, IDN_SEQ_GAP,   true,  SEQ_T1 | SEQ_T3, 0x7FFE - 31 },
    };
    run_seq_case("half-range ambiguity", half_range, sizeof(half_range) / sizeof(half_range[0]));

    static const seq_step_t stale_resync[] = {
        { 1000, IDN_SEQ_FIRST,  true,  0,      0 },
        { 1001, IDN_SEQ_NEXT,   true,  0,      0 },
        { 500,  IDN_SEQ_STALE,  false, SEQ_T1, 0 },
        { 501,  IDN_SEQ_STALE,  false, SEQ_T1, 0 },
        { 502,  IDN_SEQ_STALE,  false, SEQ_T1, 0 },
        { 503,  IDN_SEQ_RESYNC, true,  SEQ_T1, 0 },
        { 504,  IDN_SEQ_NEXT,   true,  0,      0 },
        { 1002, IDN_SEQ_GAP,    true,  SEQ_T1 | SEQ_T3, 497 - 31 },
    };
    run_seq_case("stale run resyncs", stale_resync, sizeof(stale_resync) / sizeof(stale_resync[0]));

    static const seq_step_t stale_scattered[] = {
        { 1000, IDN_SEQ_FIRST, true,  0,      0 },
        { 500,  IDN_SEQ_STALE, false, SEQ_T1, 0 },
        { 700,  IDN_SEQ_STALE, false, SEQ_T1, 0 },
        { 701,  IDN_SEQ_STALE, false, SEQ_T1, 0 },
        { 1001, IDN_SEQ_NEXT,  true,  0,      0 },
        { 702,  IDN_SEQ_STALE, false, SEQ_T1, 0 },
        { 703,  IDN_SEQ_STALE, false, SEQ_T1, 0 },
    };
    run_seq_case("scattered stale does not resync", stale_scattered,
                 sizeof(stale_scattered) / sizeof(stale_scattered[0]));

    static const seq_step_t outside_window[] = {
        { 100, IDN_SEQ_FIRST, true,  0,      0 },
        { 99,  IDN_SEQ_STALE, false, SEQ_T1, 0 },   /* before the baseline */
        { 200, IDN_SEQ_GAP,   true,  SEQ_T1 | SEQ_T3, 99 - 31 },
        { 168, IDN_SEQ_STALE, false, SEQ_T1, 0 },   /* 32 back: outside the window */
        { 169, IDN_SEQ_LATE,  false, SEQ_T1, 0 },   /* 31 back: inside */
    };
    run_seq_case("window edges", outside_window, sizeof(outside_window) / sizeof(outside_window[0]));
}

/* -- Chronology ------------------------------------------------------------ */

static void test_chronology(void)
{
    idn_chronology_t c;
    uint64_t u0 = 0, u1 = 0;

    idn_chronology_reset(&c);
    CHECK_EQ(idn_chronology_check(&c, 0xFFFFFFF0u, &u0), IDN_TS_FIRST);
    idn_chronology_commit(&c, u0);
    CHECK_EQ(idn_chronology_check(&c, 0x00000010u, &u1), IDN_TS_FORWARD);
    CHECK_EQ(u1 - u0, 0x20);
    idn_chronology_commit(&c, u1);

    /* Equal timestamps (e.g. Void messages) are legal chronology. */
    idn_chronology_reset(&c);
    CHECK_EQ(idn_chronology_check(&c, 5000, &u0), IDN_TS_FIRST);
    idn_chronology_commit(&c, u0);
    CHECK_EQ(idn_chronology_check(&c, 5000, &u1), IDN_TS_EQUAL);
    idn_chronology_commit(&c, u1);
    CHECK_EQ(idn_chronology_check(&c, 5000, &u1), IDN_TS_EQUAL);
    CHECK_EQ(u1, u0);

    /* Earlier timestamp: BEFORE, and an uncommitted reject keeps the baseline. */
    CHECK_EQ(idn_chronology_check(&c, 4999, &u1), IDN_TS_BEFORE);
    CHECK_EQ(u0 - u1, 1);
    CHECK_EQ(idn_chronology_check(&c, 5001, &u1), IDN_TS_FORWARD);
    CHECK_EQ(u1 - u0, 1);

    /* Half-range assumption. */
    idn_chronology_reset(&c);
    idn_chronology_check(&c, 0, &u0);
    idn_chronology_commit(&c, u0);
    CHECK_EQ(idn_chronology_check(&c, 0x7FFFFFFFu, &u1), IDN_TS_FORWARD);
    CHECK_EQ(idn_chronology_check(&c, 0x80000000u, &u1), IDN_TS_BEFORE);

    /* Only non-fragment chunk types carry a timestamp. */
    CHECK(idn_chunk_has_timestamp(0x00));
    CHECK(idn_chunk_has_timestamp(0x01));
    CHECK(idn_chunk_has_timestamp(0xBF));
    CHECK(!idn_chunk_has_timestamp(0xC0));
    CHECK(!idn_chunk_has_timestamp(0xFF));

    CHECK_EQ(idn_ts_unwrap(0x1FFFFFFF0ull, 0x10u), 0x200000010ull);
    CHECK_EQ(idn_ts_unwrap(0x200000010ull, 0xFFFFFFF0u), 0x1FFFFFFF0ull);
}

/* -- Wave continuity ------------------------------------------------------- */

static void test_wave(void)
{
    idn_chronology_t c;
    idn_wave_timeline_t w;
    uint64_t ts;

    /* 0xFFFFFFF0 + 32 continues at 0x10. */
    idn_chronology_reset(&c);
    idn_wave_reset(&w);
    idn_chronology_check(&c, 0xFFFFFFF0u, &ts);
    idn_chronology_commit(&c, ts);
    CHECK_EQ(idn_wave_check(&w, ts), IDN_WAVE_START);
    idn_wave_commit(&w, ts, 32);
    CHECK_EQ(idn_chronology_check(&c, 0x10u, &ts), IDN_TS_FORWARD);
    idn_chronology_commit(&c, ts);
    CHECK_EQ(idn_wave_check(&w, ts), IDN_WAVE_CONTINUES);
    idn_wave_commit(&w, ts, 2133);

    /* Void messages at an equal timestamp are chronology only; the wave
       timeline is not touched, so a wave chunk at that same timestamp
       overlaps the prior chunk's nonzero duration. */
    CHECK_EQ(idn_chronology_check(&c, 0x10u, &ts), IDN_TS_EQUAL);
    idn_chronology_commit(&c, ts);
    CHECK_EQ(idn_wave_check(&w, ts), IDN_WAVE_OVERLAP);

    /* Exact next timestamp is continuous; a repeated configuration on a
       continuous channel changes nothing. */
    idn_chronology_check(&c, 0x10u + 2133u, &ts);
    idn_wave_recover(&w);
    CHECK_EQ(idn_wave_check(&w, ts), IDN_WAVE_CONTINUES);
    idn_wave_commit(&w, ts, 2134);
    idn_chronology_commit(&c, ts);

    /* Wrong next timestamp: GAP, then drop until a recovery boundary. */
    uint64_t expected = ts + 2134;
    CHECK_EQ(idn_wave_check(&w, expected + 2133), IDN_WAVE_GAP);
    CHECK(idn_wave_verdict_is_error(IDN_WAVE_GAP));
    idn_wave_break(&w);
    CHECK_EQ(idn_wave_check(&w, expected + 2 * 2133), IDN_WAVE_AWAIT_RECOVERY);
    CHECK(idn_wave_verdict_is_error(IDN_WAVE_AWAIT_RECOVERY));
    CHECK_EQ(idn_wave_check(&w, expected), IDN_WAVE_AWAIT_RECOVERY);

    /* Repeated (identical) configuration is the recovery boundary. */
    idn_wave_recover(&w);
    CHECK_EQ(idn_wave_check(&w, expected + 3 * 2133), IDN_WAVE_RESTART);
    CHECK(!idn_wave_verdict_is_error(IDN_WAVE_RESTART));
    idn_wave_commit(&w, expected + 3 * 2133, 2133);
    CHECK_EQ(idn_wave_check(&w, expected + 4 * 2133), IDN_WAVE_CONTINUES);
    CHECK_EQ(idn_wave_check(&w, expected + 4 * 2133 - 1), IDN_WAVE_OVERLAP);

    /* Ending an epoch without error, then a new one. */
    idn_wave_end(&w);
    CHECK_EQ(idn_wave_check(&w, 12345), IDN_WAVE_RESTART);

    /* Reset (new connection) forgets prior epochs. */
    idn_wave_reset(&w);
    CHECK_EQ(idn_wave_check(&w, 12345), IDN_WAVE_START);
}

/* -- Rate policy ----------------------------------------------------------- */

static const idn_rate_policy_t POLICY_30K = { 1000, 30000, 1 };

/* Feed `chunks` chunks of `n` samples on a cumulatively rounded timeline at
   rate_hz (as a correct sender does). Returns the first non-OK verdict, or OK. */
static idn_rate_verdict_t feed_cumulative(idn_rate_epoch_t *e, const idn_rate_policy_t *p,
                                          uint32_t n, uint32_t rate_hz, uint32_t chunks,
                                          uint32_t *bad_index)
{
    uint64_t prev = 0;
    for (uint32_t k = 1; k <= chunks; k++) {
        uint64_t boundary = ((uint64_t)k * n * 1000000ull * 2u + rate_hz) / (2ull * rate_hz);
        uint32_t d = (uint32_t)(boundary - prev);
        prev = boundary;
        idn_rate_verdict_t v = idn_rate_check(p, e, n, d, 0);
        if (v != IDN_RATE_OK) {
            if (bad_index) *bad_index = k;
            return v;
        }
        idn_rate_commit(e, n, d);
    }
    return IDN_RATE_OK;
}

static void test_rate(void)
{
    idn_rate_epoch_t e;
    uint32_t bad = 0;

    /* Invalid inputs. */
    idn_rate_reset(&e);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 0, 2133, 0), IDN_RATE_INVALID);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 0, 0), IDN_RATE_INVALID);
    CHECK_EQ(idn_rate_select(&POLICY_30K, &e), 0);
    CHECK_EQ(idn_rate_requested_mhz(&e), 0);

    /* 20 samples over 40 ms = 500 Hz: below minimum. */
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 20, 40000, 0), IDN_RATE_BELOW_MIN);
    /* Extremes do not overflow. */
    /* 65535 samples over 16.78 s is 3,906 Hz. */
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 65535, 0xFFFFFF, 0), IDN_RATE_OK);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 65535, 3000000, 0), IDN_RATE_OK);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 65535, 1, 0), IDN_RATE_ABOVE_LIMIT);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 20, 0xFFFFFF, 0), IDN_RATE_BELOW_MIN);

    /* Correct 30 kpps sender: 64 samples, boundaries 2133/2133/2134 us. */
    idn_rate_reset(&e);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2133, 0), IDN_RATE_OK);
    CHECK_EQ(feed_cumulative(&e, &POLICY_30K, 64, 30000, 20000, &bad), IDN_RATE_OK);
    CHECK_EQ(idn_rate_select(&POLICY_30K, &e), 30000);

    /* Strict 2133/2134 alternation (29,997.7 pps) at a 30k limit. */
    idn_rate_reset(&e);
    {
        idn_rate_verdict_t first_bad = IDN_RATE_OK;
        for (uint32_t k = 0; k < 20000; k++) {
            uint32_t d = (k & 1u) ? 2134u : 2133u;
            idn_rate_verdict_t v = idn_rate_check(&POLICY_30K, &e, 64, d, 0);
            if (v != IDN_RATE_OK && first_bad == IDN_RATE_OK) first_bad = v;
            idn_rate_commit(&e, 64, d);
        }
        CHECK_EQ(first_bad, IDN_RATE_OK);
        CHECK_EQ(idn_rate_select(&POLICY_30K, &e), 29998);
        /* Locked at 30,000 after the prebuffer: both durations still fit. */
        CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2133, 30000), IDN_RATE_OK);
        CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2134, 30000), IDN_RATE_OK);
    }

    /* Constant 2133 us per 64 samples is 30,004.7 pps: sustained above the
       limit, rejected on the 4th chunk (64e6*k > 30000*(2133k + 1) for k > 3). */
    idn_rate_reset(&e);
    {
        idn_rate_verdict_t v = IDN_RATE_OK;
        uint32_t k;
        for (k = 1; k <= 10; k++) {
            v = idn_rate_check(&POLICY_30K, &e, 64, 2133, 0);
            if (v != IDN_RATE_OK) break;
            idn_rate_commit(&e, 64, 2133);
        }
        CHECK_EQ(v, IDN_RATE_ABOVE_LIMIT);
        CHECK_EQ(k, 4);
        /* What was accepted is played no faster than the limit. */
        CHECK_EQ(idn_rate_select(&POLICY_30K, &e), 30000);
        CHECK_EQ(idn_rate_requested_mhz(&e), 30004688);
    }

    /* Genuine 30,100 pps: rejected on the first chunk. */
    idn_rate_reset(&e);
    bad = 0;
    CHECK_EQ(feed_cumulative(&e, &POLICY_30K, 64, 30100, 100, &bad), IDN_RATE_ABOVE_LIMIT);
    CHECK_EQ(bad, 1);
    CHECK_EQ(e.samples, 0);

    /* Genuine 50,000 pps: rejected, nothing committed. */
    idn_rate_reset(&e);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 1280, 0), IDN_RATE_ABOVE_LIMIT);

    /* The limit is the policy's own: 50 kpps passes only a 50 kpps limit. */
    {
        static const idn_rate_policy_t policy_50k = { 1000, 50000, 1 };
        idn_rate_reset(&e);
        CHECK_EQ(feed_cumulative(&e, &policy_50k, 100, 50000, 1000, NULL), IDN_RATE_OK);
        CHECK_EQ(idn_rate_select(&policy_50k, &e), 50000);
    }

    /* In-stream genuine change 20,000 -> 25,000 pps: CHANGED. */
    idn_rate_reset(&e);
    CHECK_EQ(feed_cumulative(&e, &POLICY_30K, 64, 20000, 100, NULL), IDN_RATE_OK);
    CHECK_EQ(idn_rate_requested_mhz(&e), 20000000);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2560, 0), IDN_RATE_CHANGED);
    /* Small genuine change 20,000 -> 20,100.5 pps is also rejected, not ignored. */
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3184, 0), IDN_RATE_CHANGED);
    /* Downward change too. */
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3300, 0), IDN_RATE_CHANGED);
    /* One microsecond of quantization either way is not a change. */
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3201, 0), IDN_RATE_OK);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3199, 0), IDN_RATE_OK);

    /* Locked output rate: a fresh epoch at another rate is CHANGED. */
    idn_rate_reset(&e);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3200, 30000), IDN_RATE_CHANGED);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 3200, 20000), IDN_RATE_OK);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2133, 30000), IDN_RATE_OK);
    CHECK_EQ(idn_rate_check(&POLICY_30K, &e, 64, 2134, 30000), IDN_RATE_OK);

    /* Low sample counts widen the envelope: 20 samples at 30k = 666.67 us. */
    idn_rate_reset(&e);
    CHECK_EQ(feed_cumulative(&e, &POLICY_30K, 20, 30000, 3000, NULL), IDN_RATE_OK);
    CHECK_EQ(idn_rate_select(&POLICY_30K, &e), 30000);
}

/* -- Event latch ----------------------------------------------------------- */

static void test_events(void)
{
    idn_event_latch_t l;
    idn_event_report_t r;

    /* Multiple events since the last report are all present. */
    idn_event_latch_reset(&l);
    idn_event_latch_record(&l, IDN_PLEV_CTYERR);
    idn_event_latch_record(&l, IDN_PLEV_RGUERR);
    idn_event_latch_record(&l, IDN_PLEV_CTYERR);
    r = idn_event_latch_snapshot(&l);
    CHECK_EQ(r.flags, IDN_PLEV_CTYERR | IDN_PLEV_RGUERR);
    idn_event_latch_reported(&l, &r);
    CHECK_EQ(idn_event_latch_snapshot(&l).flags, 0);

    /* The same event recurring while the ACK is being sent stays latched;
       a different new event too. */
    idn_event_latch_reset(&l);
    idn_event_latch_record(&l, IDN_PLEV_CTYERR | IDN_PLEV_RGUERR);
    r = idn_event_latch_snapshot(&l);
    idn_event_latch_record(&l, IDN_PLEV_CTYERR);
    idn_event_latch_record(&l, IDN_PLEV_DCMERR);
    idn_event_latch_reported(&l, &r);
    r = idn_event_latch_snapshot(&l);
    CHECK_EQ(r.flags, IDN_PLEV_CTYERR | IDN_PLEV_DCMERR);

    /* Failed send: nothing cleared. */
    idn_event_latch_reset(&l);
    idn_event_latch_record(&l, IDN_INEV_NEW | SEQ_T1);
    r = idn_event_latch_snapshot(&l);
    (void)r;    /* send failed: no reported() */
    CHECK_EQ(idn_event_latch_snapshot(&l).flags, IDN_INEV_NEW | SEQ_T1);

    /* A report from an older snapshot clears nothing. */
    idn_event_latch_reset(&l);
    idn_event_latch_record(&l, IDN_INEV_BPERR);
    idn_event_report_t old = idn_event_latch_snapshot(&l);
    idn_event_report_t cur = idn_event_latch_snapshot(&l);
    idn_event_latch_reported(&l, &old);
    CHECK_EQ(l.flags, IDN_INEV_BPERR);
    idn_event_latch_reported(&l, &cur);
    CHECK_EQ(l.flags, 0);

    /* Channel latches are independent: reporting one leaves the other. */
    {
        idn_event_latch_t ch[2];
        idn_event_latch_reset(&ch[0]);
        idn_event_latch_reset(&ch[1]);
        idn_event_latch_record(&ch[0], IDN_PLEV_CTYERR);
        idn_event_latch_record(&ch[1], IDN_PLEV_SMERR);
        r = idn_event_latch_snapshot(&ch[0]);
        idn_event_latch_reported(&ch[0], &r);
        CHECK_EQ(ch[0].flags, 0);
        CHECK_EQ(ch[1].flags, IDN_PLEV_SMERR);
    }
}

/* -- Serialization --------------------------------------------------------- */

static void test_serialize(void)
{
    uint8_t out[IDN_ACK_STRUCT_SIZE];
    idn_ack_fields_t f;

    memset(&f, 0, sizeof(f));
    f.result_code          = IDN_ACK_RESULT_OK;
    f.input_event_flags    = IDN_INEV_SEQ_NOT_NEXT | IDN_INEV_SEQ_DUPLICATE | IDN_INEV_NEW;
    f.pipeline_event_flags = IDN_PLEV_CTYERR | IDN_PLEV_CNLCLSD;
    f.status_flags         = IDN_STATUS_SOCNL | IDN_STATUS_SNMSG;
    {
        static const uint8_t expect[12] = {
            0x0C, 0x00, 0x00, 0x31, 0x04, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        CHECK_EQ(idn_ack_serialize(&f, out), 12);
        CHECK(memcmp(out, expect, sizeof(expect)) == 0);
    }

    f.result_code = IDN_ACK_RESULT_EMPTY_CLOSE;
    f.input_event_flags = 0x8000;
    f.pipeline_event_flags = 0x0001;
    f.status_flags = 0;
    f.link_quality = 0x7F;
    f.latency_us = 0x01020304u;
    {
        static const uint8_t expect[12] = {
            0x0C, 0xEB, 0x80, 0x00, 0x00, 0x01, 0x00, 0x7F, 0x01, 0x02, 0x03, 0x04
        };
        idn_ack_serialize(&f, out);
        CHECK(memcmp(out, expect, sizeof(expect)) == 0);
    }

    /* Empty RT ACK has no channel: reset value, whatever is latched. */
    CHECK_EQ(idn_ack_pipeline_flags(IDN_PLEV_CTYERR, false, true), 0x0000);
    /* CnlClsd comes from state only. */
    CHECK_EQ(idn_ack_pipeline_flags(IDN_PLEV_CTYERR, true, true), 0x0402);
    CHECK_EQ(idn_ack_pipeline_flags(IDN_PLEV_CTYERR | IDN_PLEV_CNLCLSD, true, false), 0x0400);
    CHECK_EQ(idn_ack_pipeline_flags(IDN_PLEV_CNLRTED, true, false), 0x0001);

    /* Drained session with device data still pending: SNMsg clear, DOBuf set. */
    CHECK_EQ(idn_ack_status_flags(true, false, true), 0x03);
    CHECK_EQ(idn_ack_status_flags(false, true, false), 0x04);
    CHECK_EQ(idn_ack_status_flags(false, false, false), 0x00);
    CHECK_EQ(idn_ack_status_flags(true, true, true), 0x07);

    /* Scan status: the sole session held -> OCPD. */
    {
        idn_scan_state_t s;
        memset(&s, 0, sizeof(s));
        s.rt_supported = true;
        s.sessions_total = 1;
        s.sessions_in_use = 0;
        CHECK_EQ(idn_scan_status(&s), 0x01);
        s.sessions_in_use = 1;
        CHECK_EQ(idn_scan_status(&s), 0x11);
        s.group_excluded = true;
        CHECK_EQ(idn_scan_status(&s), 0x31);
        s.offline = true;
        s.malfunction = true;
        CHECK_EQ(idn_scan_status(&s), 0xF1);
    }
}

int main(void)
{
    test_seq();
    test_chronology();
    test_wave();
    test_rate();
    test_events();
    test_serialize();

    if (s_failures) {
        printf("idn_timing: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("idn_timing: all %d checks passed\n", s_checks);
    return 0;
}
