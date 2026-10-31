#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/gpio.h"

#include "ring_buffer.h"
#include "dac80508.h"
#include "output_engine.h"
#include "stream_rx.h"
#include "project_config.h"

#define OUTPUT_DEFAULT_SAMPLE_RATE_HZ 30000
#define OUTPUT_DEFAULT_MAX_RATE_HZ    30000
#define OUTPUT_ABSOLUTE_MAX_RATE_HZ   200000
#define OUTPUT_MIN_MAX_RATE_HZ        1000

#define OUTPUT_BATCH_SAMPLES DAC_BATCH_MAX_SAMPLES
#define BATCH_MODE_MIN_RATE_HZ 5000

#define FAILSAFE_TIMEOUT_US 20000
#define FAILSAFE_REPEAT_US 5000

/*
   Blanked X/Y moves between the parked centre and the stream take this long
   at the current rate. A one-sample step to a far point is a square-wave
   input to the scanner amplifiers; a 10 ms ramp is an ordinary blanked move.
*/
#define SOFT_MOVE_US 10000
#define SOFT_MOVE_MIN_SAMPLES 16
#define SOFT_MOVE_MAX_SAMPLES 1024

/* Steps of this size or more between consecutive samples are counted. */
#define BIG_STEP_THRESHOLD 8192

/* Laser channels above this are treated as lit for the step counters. */
#define LIT_THRESHOLD (INT16_MIN + 2048)

/* How long STOP waits for the output core to finish its current batch. */
#define CORE1_IDLE_WAIT_US 50000

/*
   Colour shift: the scanners lag the commanded position by 100 us or more,
   the laser does not. Delaying the colour channels by that lag lines the
   drawn colour up with where the beam really is. Stored in microseconds so
   the setting survives rate changes; applied as whole samples, up to this
   many. The history always records the stream, so the delay can change
   while drawing; it moves one sample per output sample toward the target,
   which never makes X/Y jump more than one sample along the path.
*/
#define COLOR_SHIFT_MAX_US 2000
#define COLOR_SHIFT_MAX_SAMPLES 128
#define COLOR_SHIFT_HIST 256   /* power of two, > COLOR_SHIFT_MAX_SAMPLES */

static volatile bool s_running = false;
static volatile bool s_core1_idle = true;
static volatile uint32_t s_sample_rate_hz = OUTPUT_DEFAULT_SAMPLE_RATE_HZ;
static volatile uint32_t s_requested_rate_hz = OUTPUT_DEFAULT_SAMPLE_RATE_HZ;
static volatile uint32_t s_max_rate_hz = OUTPUT_DEFAULT_MAX_RATE_HZ;
static volatile bool s_mirror_x = false;
static volatile bool s_mirror_y = true;

static volatile uint32_t s_consumed_samples = 0;
static volatile uint32_t s_underruns = 0;
static volatile uint32_t s_failsafe_blank_events = 0;
static volatile uint32_t s_mode_transitions = 0;
static volatile output_engine_mode_t s_output_mode = OUTPUT_MODE_PARKED;

static volatile int32_t s_color_shift_us = 0;
static volatile uint32_t s_color_shift_samples = 0;   /* |shift| in samples */
static volatile int32_t s_shift_target = 0;   /* signed samples; set by core 0 */
static int32_t s_shift_cur = 0;               /* signed samples; core 1 only */
static int16_t s_shift_hist[COLOR_SHIFT_HIST][RP_SAMPLE_CHANNELS];
static uint32_t s_shift_pos = 0;

static volatile uint32_t s_max_lit_step = 0;
static volatile uint32_t s_lit_big_steps = 0;
static volatile uint32_t s_blank_big_steps = 0;

static uint32_t s_stream_valid_samples = 0;   /* since START, core 1 only */
static output_lit_jump_t s_lit_jumps[OUTPUT_LIT_JUMP_LOG];
static volatile uint32_t s_lit_jump_serial = 0;

static int16_t s_last_output[RP_SAMPLE_CHANNELS];

static critical_section_t s_output_cs;

static int16_t s_batch[OUTPUT_BATCH_SAMPLES][RP_SAMPLE_CHANNELS];

static absolute_time_t s_last_valid_sample_time;
static absolute_time_t s_last_forced_blank_time;

static volatile bool s_core1_launched = false;

static bool s_have_valid_xy = false;
static int16_t s_last_valid_x = 0;
static int16_t s_last_valid_y = 0;

/* Soft start: blanked move from centre to the first point of a stream. */
static uint32_t s_preroll_total = 0;
static uint32_t s_preroll_done = 0;
static int16_t s_preroll_to_x = 0;
static int16_t s_preroll_to_y = 0;
static bool s_preroll_armed = false;   /* one ramp per START, then the stream plays */

static void trace_pins_init(void)
{
    /*
       Scope trace outputs on the RP_IO10/RP_IO11 breakout nets.
       OUTPUT_RUNNING is high while START is active.
       FORCED_BLANK is high whenever the DAC is not playing stream samples.
    */
    gpio_init(PIN_TRACE_OUTPUT_RUNNING);
    gpio_set_dir(PIN_TRACE_OUTPUT_RUNNING, GPIO_OUT);
    gpio_put(PIN_TRACE_OUTPUT_RUNNING, false);

    gpio_init(PIN_TRACE_FORCED_BLANK);
    gpio_set_dir(PIN_TRACE_FORCED_BLANK, GPIO_OUT);
    gpio_put(PIN_TRACE_FORCED_BLANK, true);
}

static void set_trace_running(bool running)
{
    gpio_put(PIN_TRACE_OUTPUT_RUNNING, running);
}

static void set_output_mode(output_engine_mode_t mode)
{
    bool changed = false;

    critical_section_enter_blocking(&s_output_cs);

    if (s_output_mode != mode) {
        s_output_mode = mode;
        s_mode_transitions++;
        changed = true;
    }

    critical_section_exit(&s_output_cs);

    if (changed) {
        gpio_put(PIN_TRACE_FORCED_BLANK, mode != OUTPUT_MODE_STREAMING);
    }
}

static void note_output_starved(void)
{
    /*
       START_BLANK and FAILSAFE_BLANK persist until real samples arrive.
       Only a running stream drops into UNDERRUN_BLANK.
    */
    if (s_output_mode == OUTPUT_MODE_STREAMING) {
        set_output_mode(OUTPUT_MODE_UNDERRUN_BLANK);
    }
}

static int16_t lerp_i16(int16_t from, int16_t to, uint32_t step, uint32_t steps)
{
    if (steps == 0 || step >= steps) {
        return to;
    }

    int32_t delta = (int32_t)to - (int32_t)from;
    return (int16_t)((int32_t)from + (int32_t)(((int64_t)delta * (int64_t)step) / (int64_t)steps));
}

static uint32_t soft_move_samples(void)
{
    uint32_t n = (uint32_t)(((uint64_t)s_sample_rate_hz * SOFT_MOVE_US) / 1000000ULL);

    if (n < SOFT_MOVE_MIN_SAMPLES) {
        n = SOFT_MOVE_MIN_SAMPLES;
    }

    if (n > SOFT_MOVE_MAX_SAMPLES) {
        n = SOFT_MOVE_MAX_SAMPLES;
    }

    return n;
}

static void sample_blanked_at(int16_t sample[RP_SAMPLE_CHANNELS], int16_t x, int16_t y)
{
    sample[0] = x;
    sample[1] = y;

    /* Laser/colour/intensity and aux channels raw 0x0000. */
    for (int i = 2; i < RP_SAMPLE_CHANNELS; i++) {
        sample[i] = INT16_MIN;
    }
}

static void sample_parked(int16_t sample[RP_SAMPLE_CHANNELS])
{
    /*
       Parked: scanners at centre (signed 0 = raw 0x8000 = 0 V on the ILDA
       X/Y pairs), every laser channel at raw 0x0000 = 0 V.
    */
    sample_blanked_at(sample, 0, 0);
}

static int16_t mirror_i16(int16_t v)
{
    return (v == INT16_MIN) ? INT16_MAX : (int16_t)-v;
}

static void apply_output_mirror(int16_t sample[RP_SAMPLE_CHANNELS])
{
    if (s_mirror_x) {
        sample[0] = mirror_i16(sample[0]);
    }

    if (s_mirror_y) {
        sample[1] = mirror_i16(sample[1]);
    }
}

static void apply_xy_mirror(int16_t *x, int16_t *y)
{
    if (s_mirror_x) {
        *x = mirror_i16(*x);
    }

    if (s_mirror_y) {
        *y = mirror_i16(*y);
    }
}

static void get_hold_xy(int16_t *x, int16_t *y)
{
    if (s_have_valid_xy) {
        *x = s_last_valid_x;
        *y = s_last_valid_y;
    } else {
        /* No real point since START: hold the centre, where STOP parked. */
        *x = 0;
        *y = 0;
    }
}

static void sample_blanked_hold_xy(int16_t sample[RP_SAMPLE_CHANNELS])
{
    int16_t x;
    int16_t y;

    get_hold_xy(&x, &y);
    sample_blanked_at(sample, x, y);
}

static void dac_write_blanked_xy_mirrored(int16_t x, int16_t y)
{
    apply_xy_mirror(&x, &y);
    dac_write_blanked_xy(x, y);
}

static void set_last_output_parked(void)
{
    critical_section_enter_blocking(&s_output_cs);
    sample_parked(s_last_output);
    critical_section_exit(&s_output_cs);
}

static void set_last_output_blanked_hold_xy(void)
{
    int16_t sample[RP_SAMPLE_CHANNELS];

    sample_blanked_hold_xy(sample);
    apply_output_mirror(sample);

    critical_section_enter_blocking(&s_output_cs);
    memcpy(s_last_output, sample, sizeof(sample));
    critical_section_exit(&s_output_cs);
}

static bool sample_is_lit(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    for (int i = 2; i <= 5; i++) {
        if (sample[i] > LIT_THRESHOLD) {
            return true;
        }
    }

    return false;
}

static void note_valid_sample(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    if (s_have_valid_xy) {
        uint32_t dx = (uint32_t)abs((int32_t)sample[0] - (int32_t)s_last_valid_x);
        uint32_t dy = (uint32_t)abs((int32_t)sample[1] - (int32_t)s_last_valid_y);
        uint32_t step = dx > dy ? dx : dy;

        if (sample_is_lit(sample)) {
            if (step > s_max_lit_step) {
                s_max_lit_step = step;
            }
            if (step >= BIG_STEP_THRESHOLD) {
                s_lit_big_steps++;

                uint32_t serial = s_lit_jump_serial + 1u;
                output_lit_jump_t *j = &s_lit_jumps[serial % OUTPUT_LIT_JUMP_LOG];

                j->serial = serial;
                j->sample = s_stream_valid_samples;
                j->pos = s_stream_valid_samples % STREAM_SAMPLES_PER_PACKET;
                j->from_x = s_last_valid_x;
                j->from_y = s_last_valid_y;
                j->to_x = sample[0];
                j->to_y = sample[1];
                __atomic_store_n(&s_lit_jump_serial, serial, __ATOMIC_RELEASE);
            }
        } else if (step >= BIG_STEP_THRESHOLD) {
            s_blank_big_steps++;
        }
    }

    s_last_valid_x = sample[0];
    s_last_valid_y = sample[1];
    s_have_valid_xy = true;
    s_stream_valid_samples++;
    s_last_valid_sample_time = get_absolute_time();
}

static void force_dac_blanked_hold_xy_now(void)
{
    int16_t x;
    int16_t y;

    get_hold_xy(&x, &y);

    set_output_mode(OUTPUT_MODE_FAILSAFE_BLANK);

    critical_section_enter_blocking(&s_output_cs);
    s_failsafe_blank_events++;
    critical_section_exit(&s_output_cs);

    set_last_output_blanked_hold_xy();
    dac_write_blanked_xy_mirrored(x, y);

    s_last_forced_blank_time = get_absolute_time();
}

static void failsafe_check_no_samples(void)
{
    absolute_time_t now = get_absolute_time();

    int64_t since_valid_us = absolute_time_diff_us(s_last_valid_sample_time, now);

    if (since_valid_us < FAILSAFE_TIMEOUT_US) {
        return;
    }

    int64_t since_blank_us = absolute_time_diff_us(s_last_forced_blank_time, now);

    if (since_blank_us >= FAILSAFE_REPEAT_US) {
        force_dac_blanked_hold_xy_now();
    }
}

/* -- Colour shift delay line ------------------------------------------------ */

/* Recompute the target delay in samples from the time and the rate. */
static void color_shift_update_target(void)
{
    int32_t shift_us = s_color_shift_us;
    uint32_t mag_us = (uint32_t)(shift_us < 0 ? -shift_us : shift_us);
    uint32_t n = (uint32_t)(((uint64_t)mag_us * s_sample_rate_hz + 500000ULL) / 1000000ULL);

    if (n > COLOR_SHIFT_MAX_SAMPLES) {
        n = COLOR_SHIFT_MAX_SAMPLES;
    }

    s_color_shift_samples = n;
    s_shift_target = shift_us < 0 ? -(int32_t)n : (int32_t)n;
}

/*
   Restart the history from `seed` (a pre-mirror sample) at the target
   delay. Only while core 1 is not applying the shift: before START.
*/
static void color_shift_reseed(const int16_t seed[RP_SAMPLE_CHANNELS])
{
    for (uint32_t i = 0; i < COLOR_SHIFT_HIST; i++) {
        memcpy(s_shift_hist[i], seed, RP_SAMPLE_CHANNELS * sizeof(int16_t));
    }

    s_shift_pos = 0;
    s_shift_cur = s_shift_target;
}

/*
   Apply the colour shift to one output sample, in stream order, before the
   mirror. Positive shift: X/Y from this sample, colours from n samples ago.
   Negative shift: colours from this sample, X/Y from n samples ago.
*/
static void color_shift_apply(int16_t sample[RP_SAMPLE_CHANNELS])
{
    int32_t target = s_shift_target;

    if (s_shift_cur < target) {
        s_shift_cur++;
    } else if (s_shift_cur > target) {
        s_shift_cur--;
    }

    memcpy(s_shift_hist[s_shift_pos], sample, RP_SAMPLE_CHANNELS * sizeof(int16_t));

    int32_t d = s_shift_cur;

    if (d != 0) {
        uint32_t n = (uint32_t)(d < 0 ? -d : d);
        const int16_t *old = s_shift_hist[(s_shift_pos - n) & (COLOR_SHIFT_HIST - 1)];

        if (d > 0) {
            for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++) {
                sample[ch] = old[ch];
            }
        } else {
            sample[0] = old[0];
            sample[1] = old[1];
        }
    }

    s_shift_pos = (s_shift_pos + 1) & (COLOR_SHIFT_HIST - 1);
}

/* -- Soft start ------------------------------------------------------------- */

static void preroll_reset(void)
{
    s_preroll_total = 0;
    s_preroll_done = 0;
    s_preroll_to_x = 0;
    s_preroll_to_y = 0;
    s_preroll_armed = false;
}

static bool preroll_active(void)
{
    return s_preroll_done < s_preroll_total;
}

/*
   Before the first sample of a stream plays, look at it and queue a blanked
   ramp from the parked centre to its position. The stream itself starts
   SOFT_MOVE_US later and continues from that point without a step.
*/
static void preroll_arm_if_needed(void)
{
    int16_t first[RP_SAMPLE_CHANNELS];

    if (s_have_valid_xy || s_preroll_armed) {
        return;
    }

    if (!ring_peek(first)) {
        return;
    }

    /* Arm once per START: when the ramp ends the first sample is popped
       for real, which sets s_have_valid_xy. Without this latch the ramp
       would re-arm forever and the stream would never play. */
    s_preroll_armed = true;

    if (first[0] == 0 && first[1] == 0) {
        return;
    }

    s_preroll_to_x = first[0];
    s_preroll_to_y = first[1];
    s_preroll_total = soft_move_samples();
    s_preroll_done = 0;
}

static bool preroll_next(int16_t sample[RP_SAMPLE_CHANNELS])
{
    if (!preroll_active()) {
        return false;
    }

    s_preroll_done++;
    sample_blanked_at(sample,
                      lerp_i16(0, s_preroll_to_x, s_preroll_done, s_preroll_total),
                      lerp_i16(0, s_preroll_to_y, s_preroll_done, s_preroll_total));
    return true;
}

/* -- Output paths ----------------------------------------------------------- */

/*
   Write to the DAC and record where the scanners go, together. If STOP
   arrived while the samples were prepared, do neither: STOP waits for this
   core to go idle, then ramps from the last position really written.
*/
static void submit_sample(const int16_t *sample)
{
    if (!s_running) {
        return;
    }

    critical_section_enter_blocking(&s_output_cs);
    memcpy(s_last_output, sample, RP_SAMPLE_CHANNELS * sizeof(int16_t));
    critical_section_exit(&s_output_cs);

    dac_write_sample(sample);
}

static void submit_batch(void)
{
    if (!s_running) {
        return;
    }

    critical_section_enter_blocking(&s_output_cs);
    memcpy(s_last_output,
           s_batch[OUTPUT_BATCH_SAMPLES - 1],
           RP_SAMPLE_CHANNELS * sizeof(int16_t));
    critical_section_exit(&s_output_cs);

    dac_write_sample_batch((const int16_t (*)[RP_SAMPLE_CHANNELS])s_batch, OUTPUT_BATCH_SAMPLES);
}

static void output_one_sample_timed(absolute_time_t *next_time)
{
    int16_t sample[RP_SAMPLE_CHANNELS];

    uint32_t rate = s_sample_rate_hz;

    if (rate < 1) {
        rate = 1;
    }

    if (rate > OUTPUT_ABSOLUTE_MAX_RATE_HZ) {
        rate = OUTPUT_ABSOLUTE_MAX_RATE_HZ;
    }

    int64_t period_us = 1000000LL / rate;

    if (period_us < 5) {
        period_us = 5;
    }

    *next_time = delayed_by_us(*next_time, period_us);
    sleep_until(*next_time);

    if (!s_running) {
        return;
    }

    preroll_arm_if_needed();

    if (preroll_next(sample)) {
        color_shift_apply(sample);
        apply_output_mirror(sample);

        submit_sample(sample);
        return;
    }

    if (ring_pop(sample)) {
        set_output_mode(OUTPUT_MODE_STREAMING);
        note_valid_sample(sample);
        color_shift_apply(sample);
        apply_output_mirror(sample);

        critical_section_enter_blocking(&s_output_cs);
        s_consumed_samples++;
        critical_section_exit(&s_output_cs);

        submit_sample(sample);
    } else {
        int16_t blanked[RP_SAMPLE_CHANNELS];

        note_output_starved();
        sample_blanked_hold_xy(blanked);
        color_shift_apply(blanked);
        apply_output_mirror(blanked);

        critical_section_enter_blocking(&s_output_cs);
        s_underruns++;
        critical_section_exit(&s_output_cs);

        failsafe_check_no_samples();

        /* Underrun: blank the laser channels, hold X/Y where they are. */
        submit_sample(blanked);
    }
}

static void output_preroll_batch(void)
{
    uint32_t i = 0;

    while (i < OUTPUT_BATCH_SAMPLES && preroll_next(s_batch[i])) {
        i++;
    }

    /* The ramp ended inside this batch: wait at its end, blanked. */
    for (; i < OUTPUT_BATCH_SAMPLES; i++) {
        sample_blanked_at(s_batch[i], s_preroll_to_x, s_preroll_to_y);
    }

    for (i = 0; i < OUTPUT_BATCH_SAMPLES; i++) {
        color_shift_apply(s_batch[i]);
        apply_output_mirror(s_batch[i]);
    }

    submit_batch();
}

static void output_batch(void)
{
    uint32_t got = 0;

    if (!s_running) {
        return;
    }

    preroll_arm_if_needed();

    if (preroll_active()) {
        output_preroll_batch();
        return;
    }

    got = ring_pop_block(s_batch, OUTPUT_BATCH_SAMPLES);

    for (uint32_t i = 0; i < got; i++) {
        note_valid_sample(s_batch[i]);
        color_shift_apply(s_batch[i]);
        apply_output_mirror(s_batch[i]);
    }

    for (uint32_t i = got; i < OUTPUT_BATCH_SAMPLES; i++) {
        sample_blanked_hold_xy(s_batch[i]);
        color_shift_apply(s_batch[i]);
        apply_output_mirror(s_batch[i]);
    }

    if (got > 0) {
        s_last_valid_sample_time = get_absolute_time();
    }

    uint32_t missing = OUTPUT_BATCH_SAMPLES - got;

    if (got == 0) {
        note_output_starved();
    } else {
        set_output_mode(missing > 0 ? OUTPUT_MODE_UNDERRUN_BLANK : OUTPUT_MODE_STREAMING);
    }

    critical_section_enter_blocking(&s_output_cs);
    s_consumed_samples += got;
    s_underruns += missing;
    critical_section_exit(&s_output_cs);

    if (missing > 0) {
        failsafe_check_no_samples();
    }

    /* Missing samples inside the batch are blanked-hold-XY samples. */
    submit_batch();
}

/* -- Start / stop ----------------------------------------------------------- */

static void wait_core1_idle(void)
{
    if (!s_core1_launched) {
        return;
    }

    absolute_time_t deadline = make_timeout_time_us(CORE1_IDLE_WAIT_US);

    while (!s_core1_idle && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        tight_loop_contents();
    }
}

static void halt_output_core(void)
{
    s_running = false;
    set_trace_running(false);
    wait_core1_idle();
}

static void reset_stream_state(void)
{
    ring_clear();

    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;
    preroll_reset();

    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;
}

/*
   Blanked move from the last commanded X/Y (already mirrored) to the centre,
   one DAC write per sample period. Each dac_write_blanked_xy() blocks for
   exactly one sample at the current rate, so the loop paces itself.
*/
static void park_ramp(int16_t from_x, int16_t from_y)
{
    if (from_x == 0 && from_y == 0) {
        return;
    }

    uint32_t n = soft_move_samples();

    for (uint32_t i = 1; i <= n; i++) {
        dac_write_blanked_xy(lerp_i16(from_x, 0, i, n), lerp_i16(from_y, 0, i, n));
    }
}

static void output_core_main(void)
{
    absolute_time_t next_time = get_absolute_time();

    flash_safe_execute_core_init();

    while (true) {
        uint32_t rate = s_sample_rate_hz;

        if (!s_running) {
            s_core1_idle = true;
            sleep_ms(1);
            next_time = get_absolute_time();
            continue;
        }

        s_core1_idle = false;

        /* STOP may have landed between the check above and the flag. */
        if (!s_running) {
            s_core1_idle = true;
            continue;
        }

        if (rate < BATCH_MODE_MIN_RATE_HZ) {
            output_one_sample_timed(&next_time);
        } else {
            output_batch();
        }
    }
}

static void launch_output_core(void)
{
    if (!s_core1_launched) {
        multicore_launch_core1(output_core_main);
        s_core1_launched = true;
    }
}

void output_engine_init(void)
{
    critical_section_init(&s_output_cs);
    trace_pins_init();

    set_last_output_parked();

    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;
    preroll_reset();

    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;

    dac_init();

    /*
       Critical boot-time sync:
       dac_init() initializes DAC timing internally, but the output engine
       owns the real output rate.
    */
    dac_set_sample_rate(s_sample_rate_hz);

    dac_write_park();

    launch_output_core();
}

void output_engine_start(void)
{
    if (s_running) {
        return;
    }

    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;

    /*
       Until the first real point arrives, keep the scanners at the centre,
       where STOP parked them, with the laser blanked. The first point is
       then reached by a blanked ramp (see preroll_arm_if_needed).
    */
    s_have_valid_xy = false;
    s_last_valid_x = 0;
    s_last_valid_y = 0;
    preroll_reset();

    s_max_lit_step = 0;
    s_lit_big_steps = 0;
    s_blank_big_steps = 0;
    s_stream_valid_samples = 0;

    {
        int16_t parked[RP_SAMPLE_CHANNELS];
        sample_parked(parked);
        color_shift_reseed(parked);
    }

    dac_write_blanked_xy(0, 0);

    set_output_mode(OUTPUT_MODE_START_BLANK);
    set_last_output_blanked_hold_xy();

    s_running = true;
    set_trace_running(true);
}

void output_engine_stop_safe(void)
{
    int16_t from_x;
    int16_t from_y;

    halt_output_core();

    critical_section_enter_blocking(&s_output_cs);
    from_x = s_last_output[0];
    from_y = s_last_output[1];
    critical_section_exit(&s_output_cs);

    reset_stream_state();

    /* Laser channels go to 0 V on the first ramp sample; X/Y walk to centre. */
    park_ramp(from_x, from_y);

    set_output_mode(OUTPUT_MODE_PARKED);
    set_last_output_parked();
    dac_write_park();
}

void output_engine_pause_for_flash(void)
{
    halt_output_core();
    reset_stream_state();
    set_output_mode(OUTPUT_MODE_PARKED);
    sleep_ms(5);
}

void output_engine_resume_after_flash(void)
{
    s_last_valid_sample_time = get_absolute_time();
    s_last_forced_blank_time = s_last_valid_sample_time;
    launch_output_core();
}

static void apply_rate_locked_by_limit(void)
{
    uint32_t rate_hz = s_requested_rate_hz;

    if (rate_hz < 1) {
        rate_hz = 1;
    }

    if (rate_hz > s_max_rate_hz) {
        rate_hz = s_max_rate_hz;
    }

    if (rate_hz > OUTPUT_ABSOLUTE_MAX_RATE_HZ) {
        rate_hz = OUTPUT_ABSOLUTE_MAX_RATE_HZ;
    }

    s_sample_rate_hz = rate_hz;
    dac_set_sample_rate(rate_hz);

    /* The delay is a time; its length in samples follows the rate. */
    color_shift_update_target();
}

void output_engine_set_rate(uint32_t rate_hz)
{
    s_requested_rate_hz = rate_hz;
    apply_rate_locked_by_limit();
}

void output_engine_set_max_rate(uint32_t max_rate_hz)
{
    if (max_rate_hz < OUTPUT_MIN_MAX_RATE_HZ) {
        max_rate_hz = OUTPUT_MIN_MAX_RATE_HZ;
    }

    if (max_rate_hz > OUTPUT_ABSOLUTE_MAX_RATE_HZ) {
        max_rate_hz = OUTPUT_ABSOLUTE_MAX_RATE_HZ;
    }

    s_max_rate_hz = max_rate_hz;
    apply_rate_locked_by_limit();
}

uint32_t output_engine_get_max_rate(void)
{
    return s_max_rate_hz;
}

void output_engine_set_color_shift(int32_t shift_us)
{
    if (shift_us < -COLOR_SHIFT_MAX_US) {
        shift_us = -COLOR_SHIFT_MAX_US;
    }

    if (shift_us > COLOR_SHIFT_MAX_US) {
        shift_us = COLOR_SHIFT_MAX_US;
    }

    s_color_shift_us = shift_us;
    color_shift_update_target();
}

int32_t output_engine_get_color_shift(void)
{
    return s_color_shift_us;
}

void output_engine_set_mirror(bool mirror_x, bool mirror_y)
{
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
}

void output_engine_get_mirror(bool *mirror_x, bool *mirror_y)
{
    if (mirror_x) {
        *mirror_x = s_mirror_x;
    }

    if (mirror_y) {
        *mirror_y = s_mirror_y;
    }
}

void output_engine_get_status(output_engine_status_t *status)
{
    if (!status) {
        return;
    }

    critical_section_enter_blocking(&s_output_cs);

    status->running = s_running;
    status->sample_rate_hz = s_sample_rate_hz;
    status->requested_rate_hz = s_requested_rate_hz;
    status->max_rate_hz = s_max_rate_hz;
    status->color_shift_us = s_color_shift_us;
    status->color_shift_samples = s_color_shift_samples;
    status->consumed_samples = s_consumed_samples;
    status->underruns = s_underruns;
    status->failsafe_blank_events = s_failsafe_blank_events;
    status->mode_transitions = s_mode_transitions;
    status->mode = s_output_mode;
    status->mirror_x = s_mirror_x;
    status->mirror_y = s_mirror_y;
    status->max_lit_step = s_max_lit_step;
    status->lit_big_steps = s_lit_big_steps;
    status->blank_big_steps = s_blank_big_steps;

    memcpy(status->last_output, s_last_output, sizeof(status->last_output));

    critical_section_exit(&s_output_cs);
}

uint32_t output_engine_get_lit_jumps(output_lit_jump_t *out, uint32_t max, uint32_t *serial)
{
    uint32_t latest = __atomic_load_n(&s_lit_jump_serial, __ATOMIC_ACQUIRE);
    uint32_t first = *serial + 1u;
    uint32_t n = 0;

    if (latest - *serial > OUTPUT_LIT_JUMP_LOG) {
        first = latest - OUTPUT_LIT_JUMP_LOG + 1u;
    }

    /* Core 1 may overwrite an entry while it is copied; diagnostics only. */
    for (uint32_t s = first; s != latest + 1u && n < max; s++) {
        out[n++] = s_lit_jumps[s % OUTPUT_LIT_JUMP_LOG];
    }

    /* Entries that did not fit are read on the next call. */
    *serial = first + n - 1u;
    return n;
}
