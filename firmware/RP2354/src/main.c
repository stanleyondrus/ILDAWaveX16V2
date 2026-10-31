#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "ring_buffer.h"
#include "rp_uart.h"
#include "stream_rx.h"
#include "output_engine.h"
#include "dac80508.h"
#include "rp_settings.h"
#include "config.h"

static const char *output_mode_name(output_engine_mode_t mode)
{
    switch (mode) {
    case OUTPUT_MODE_PARKED:
        return "PARKED";
    case OUTPUT_MODE_START_BLANK:
        return "START_BLANK";
    case OUTPUT_MODE_STREAMING:
        return "STREAMING";
    case OUTPUT_MODE_UNDERRUN_BLANK:
        return "UNDERRUN_BLANK";
    case OUTPUT_MODE_FAILSAFE_BLANK:
        return "FAILSAFE_BLANK";
    default:
        return "UNKNOWN";
    }
}

static bool channels_blanked(const int16_t sample[RP_SAMPLE_CHANNELS])
{
    for (uint32_t channel = 2; channel < RP_SAMPLE_CHANNELS; channel++) {
        if (sample[channel] != INT16_MIN) {
            return false;
        }
    }

    return true;
}

static void log_runtime_changes(void)
{
    static bool initialized = false;
    static absolute_time_t next_report;
    static output_engine_status_t previous_output;
    static stream_rx_status_t previous_rx;
    static uint32_t previous_dma_words;
    static uint32_t lit_jump_serial;

    if (!rp_log_console_connected()) {
        /* Re-baseline on reconnect so deltas cover only the attached period. */
        initialized = false;
        return;
    }

    absolute_time_t now = get_absolute_time();

    if (initialized && absolute_time_diff_us(now, next_report) > 0) {
        return;
    }

    output_engine_status_t output;
    stream_rx_status_t rx;
    output_engine_get_status(&output);
    stream_rx_get_status(&rx);

    if (!initialized) {
        previous_output = output;
        previous_rx = rx;
        previous_dma_words = dac_get_dma_words_submitted();
        next_report = delayed_by_ms(now, 1000);
        initialized = true;
        return;
    }

    next_report = delayed_by_ms(now, 1000);

    uint32_t transition_delta = output.mode_transitions - previous_output.mode_transitions;
    uint32_t underrun_delta = output.underruns - previous_output.underruns;
    uint32_t failsafe_delta = output.failsafe_blank_events - previous_output.failsafe_blank_events;
    uint32_t dma_words = dac_get_dma_words_submitted();
    uint32_t dma_words_delta = dma_words - previous_dma_words;

    if (output.running || transition_delta > 0 || underrun_delta > 0 || failsafe_delta > 0) {
        rp_log("output mode=%s transitions=+%lu underrun_samples=+%lu failsafe_blanks=+%lu consumed=%lu packets=%lu fill=%lu dma_words_to_pio_fifo=+%lu commanded_xy=(%d,%d) commanded_laser_channels_blank=%u max_lit_step=%lu lit_big_steps=%lu blank_big_steps=%lu",
               output_mode_name(output.mode),
               (unsigned long)transition_delta,
               (unsigned long)underrun_delta,
               (unsigned long)failsafe_delta,
               (unsigned long)output.consumed_samples,
               (unsigned long)rx.stream_packets,
               (unsigned long)ring_get_fill(),
               (unsigned long)dma_words_delta,
               output.last_output[0],
               output.last_output[1],
               channels_blanked(output.last_output) ? 1u : 0u,
               (unsigned long)output.max_lit_step,
               (unsigned long)output.lit_big_steps,
               (unsigned long)output.blank_big_steps);
    }

    uint32_t bad_delta = rx.stream_bad_packets - previous_rx.stream_bad_packets;
    uint32_t dropped_delta = rx.stream_dropped_packets - previous_rx.stream_dropped_packets;
    uint32_t timeout_delta = rx.stream_rx_timeouts - previous_rx.stream_rx_timeouts;
    uint32_t gap_delta = rx.seq_gaps - previous_rx.seq_gaps;
    uint32_t missing_delta = rx.seq_missing - previous_rx.seq_missing;
    uint32_t partial_delta = rx.rx_partial - previous_rx.rx_partial;

    if (bad_delta > 0 || dropped_delta > 0 || gap_delta > 0 || partial_delta > 0 ||
        (output.running && timeout_delta > 0)) {
        rp_log("stream_errors bad=+%lu dropped=+%lu rx_timeout=+%lu payload_errors=%lu seq_gaps=+%lu seq_missing=+%lu rx_partial=+%lu",
               (unsigned long)bad_delta,
               (unsigned long)dropped_delta,
               (unsigned long)(output.running ? timeout_delta : 0),
               (unsigned long)rx.payload_errors,
               (unsigned long)gap_delta,
               (unsigned long)missing_delta,
               (unsigned long)partial_delta);
    }

    output_lit_jump_t jumps[4];
    uint32_t njumps = output_engine_get_lit_jumps(jumps, 4, &lit_jump_serial);

    for (uint32_t i = 0; i < njumps; i++) {
        rp_log("lit_jump n=%lu sample=%lu pos=%lu from=(%d,%d) to=(%d,%d)",
               (unsigned long)jumps[i].serial,
               (unsigned long)jumps[i].sample,
               (unsigned long)jumps[i].pos,
               jumps[i].from_x, jumps[i].from_y,
               jumps[i].to_x, jumps[i].to_y);
    }

    previous_output = output;
    previous_rx = rx;
    previous_dma_words = dma_words;
}

#ifdef ENABLE_HELIOS_USB
#include "tusb.h"
#include "helios_protocol.h"
#endif

int main(void)
{
    stdio_init_all();
    sleep_ms(100);

    bool watchdog_reset = watchdog_caused_reboot();

    rp_settings_load();

    rp_settings_t settings;
    rp_settings_get(&settings);

    ring_init();
    ring_configure(settings.buffer_size_samples,
                   settings.buffer_high_water_samples,
                   settings.buffer_low_water_samples);
    rp_uart_init();
    stream_rx_init();
    output_engine_init();
    output_engine_set_mirror(settings.mirror_x, settings.mirror_y);
    output_engine_set_max_rate(settings.max_rate_hz);
    output_engine_set_color_shift(settings.color_shift_us);

    bool startup_logged = false;

#ifdef ENABLE_HELIOS_USB
    helios_protocol_init();
    tusb_init();
#endif

    while (true) {
        if (!startup_logged && rp_log_console_connected()) {
            rp_log("boot version=%s watchdog_reset=%u", RP_FW_VERSION, watchdog_reset ? 1u : 0u);
            rp_log("ready buffer=%lu high_water=%lu low_water=%lu mirror_x=%u mirror_y=%u max_rate=%lu color_shift_us=%ld output=PARKED",
                   (unsigned long)ring_get_capacity(),
                   (unsigned long)ring_get_high_water(),
                   (unsigned long)ring_get_low_water(),
                   settings.mirror_x ? 1u : 0u,
                   settings.mirror_y ? 1u : 0u,
                   (unsigned long)output_engine_get_max_rate(),
                   (long)output_engine_get_color_shift());
            startup_logged = true;
        }

        log_runtime_changes();

#ifdef ENABLE_HELIOS_USB
        tud_task();
        helios_protocol_task();
        rp_uart_poll();

        /*
         * SPI reception is skipped while USB is active: the main loop must
         * remain free to call tud_task() and fire the Helios tick at rate.
         * When USB is not mounted both modes are inactive and SPI runs as before.
         */
        if (!tud_mounted()) {
            if (stream_rx_receive_packet(10)) {
                stream_rx_process_packet();
            }
        } else {
            stream_rx_cancel();
        }
#else
        rp_uart_poll();
        if (stream_rx_receive_packet(10)) {
            stream_rx_process_packet();
        }
#endif
    }
}
