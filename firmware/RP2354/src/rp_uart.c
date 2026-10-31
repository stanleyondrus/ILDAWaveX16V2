#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"

#include "ring_buffer.h"
#include "stream_rx.h"
#include "output_engine.h"
#include "rp_uart.h"
#include "rp_settings.h"
#include "config.h"
#include "project_config.h"

#ifndef ENABLE_HELIOS_USB
#include "pico/stdio_usb.h"
#endif

#define RP_UART_PORT      uart1
#define RP_UART_TX_GPIO   PIN_RP_UART_TX
#define RP_UART_RX_GPIO   PIN_RP_UART_RX
#define RP_UART_BAUD_RATE 921600

bool rp_log_console_connected(void)
{
#ifndef ENABLE_HELIOS_USB
    return stdio_usb_connected();
#else
    return false;
#endif
}

void rp_log(const char *format, ...)
{
    if (!format || !rp_log_console_connected()) {
        return;
    }

    va_list args;
    va_start(args, format);
    printf("[RP] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static bool parse_u32_arg(const char *line, const char *key, uint32_t *out)
{
    const char *p = strstr(line, key);

    if (!p || !out) {
        return false;
    }

    p += strlen(key);
    *out = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

/* A SHIFT applied while running is not in flash until SHIFT save. */
static bool s_color_shift_unsaved = false;

/* LIMIT or MIRROR changed while output ran; written after the next STOP/CLEAR. */
static bool s_settings_pending = false;

/*
   A flash write pauses the output core, so callers only save while output
   is stopped. The colour shift is written only by a SHIFT save; any other
   save keeps the saved one, so a value still being tried stays unsaved.
*/
static void save_current_settings(bool include_color_shift)
{
    rp_settings_t settings;

    rp_settings_get(&settings);

    settings.buffer_size_samples       = ring_get_capacity();
    settings.buffer_high_water_samples = ring_get_high_water();
    settings.buffer_low_water_samples  = ring_get_low_water();
    output_engine_get_mirror(&settings.mirror_x, &settings.mirror_y);
    settings.max_rate_hz = output_engine_get_max_rate();

    if (include_color_shift || !s_color_shift_unsaved) {
        settings.color_shift_us = output_engine_get_color_shift();
    }

    if (rp_settings_save(&settings)) {
        s_settings_pending = false;
        if (include_color_shift) {
            s_color_shift_unsaved = false;
        }
    }
}

/* Save now if stopped, else after the next STOP/CLEAR. Returns true if saved now. */
static bool save_or_defer_settings(void)
{
    output_engine_status_t out;

    output_engine_get_status(&out);

    if (out.running) {
        s_settings_pending = true;
        return false;
    }

    save_current_settings(false);
    return true;
}

static void save_pending_settings(void)
{
    if (s_settings_pending) {
        save_current_settings(false);
        rp_log("settings=SAVED deferred");
    }
}

void rp_uart_init(void)
{
    uart_init(RP_UART_PORT, RP_UART_BAUD_RATE);

    gpio_set_function(RP_UART_TX_GPIO, GPIO_FUNC_UART);
    gpio_set_function(RP_UART_RX_GPIO, GPIO_FUNC_UART);

    uart_set_format(RP_UART_PORT, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(RP_UART_PORT, true);
}

void uart_send_line(const char *s)
{
    uart_puts(RP_UART_PORT, s);
    uart_puts(RP_UART_PORT, "\n");
    uart_tx_wait_blocking(RP_UART_PORT);
}

static void send_status(void)
{
    static char line[768];   /* core 0 only; keeps STATUS off the stack */

    stream_rx_status_t rx;
    output_engine_status_t out;

    stream_rx_get_status(&rx);
    output_engine_get_status(&out);

    snprintf(line, sizeof(line),
        "STATUS "
        "running=%lu "
        "rate=%lu "
        "fill=%lu "
        "buffer_size=%lu "
        "buffer_high_water=%lu "
        "buffer_low_water=%lu "
        "buffer_max=%lu "
        "underruns=%lu "
        "payload_errors=%lu "
        "stream_packets=%lu "
        "bad_packets=%lu "
        "dropped_packets=%lu "
        "rx_timeouts=%lu "
        "mirror_x=%lu "
        "mirror_y=%lu "
        "max_rate=%lu "
        "requested_rate=%lu "
        "max_lit_step=%lu "
        "lit_big_steps=%lu "
        "blank_big_steps=%lu "
        "stale_packets=%lu "
        "seq_gaps=%lu "
        "seq_missing=%lu "
        "rx_partial=%lu "
        "color_shift_us=%ld "
        "color_shift_samples=%lu "
        "color_shift_saved=%lu",
        out.running ? 1ul : 0ul,
        (unsigned long)out.sample_rate_hz,
        (unsigned long)ring_get_fill(),
        (unsigned long)ring_get_capacity(),
        (unsigned long)ring_get_high_water(),
        (unsigned long)ring_get_low_water(),
        (unsigned long)RP_RING_SAMPLES,
        (unsigned long)out.underruns,
        (unsigned long)rx.payload_errors,
        (unsigned long)rx.stream_packets,
        (unsigned long)rx.stream_bad_packets,
        (unsigned long)rx.stream_dropped_packets,
        (unsigned long)rx.stream_rx_timeouts,
        out.mirror_x ? 1ul : 0ul,
        out.mirror_y ? 1ul : 0ul,
        (unsigned long)out.max_rate_hz,
        (unsigned long)out.requested_rate_hz,
        (unsigned long)out.max_lit_step,
        (unsigned long)out.lit_big_steps,
        (unsigned long)out.blank_big_steps,
        (unsigned long)rx.stream_stale_packets,
        (unsigned long)rx.seq_gaps,
        (unsigned long)rx.seq_missing,
        (unsigned long)rx.rx_partial,
        (long)out.color_shift_us,
        (unsigned long)out.color_shift_samples,
        s_color_shift_unsaved ? 0ul : 1ul
    );

    uart_send_line(line);
}

static void handle_line(char *line)
{
    if (strcmp(line, "VERSION") == 0) {
        uart_send_line("VERSION " RP_FW_VERSION);
        return;
    }

    if (strcmp(line, "PING") == 0) {
        uart_send_line("PONG");
        return;
    }

    if (strcmp(line, "STATUS") == 0) {
        send_status();
        return;
    }

    if (strcmp(line, "START") == 0) {
        output_engine_start();
        rp_log("command=START output=START_BLANK xy=center channels=blank soft_start=10ms");
        uart_send_line("OK START");
        return;
    }

    if (strcmp(line, "STOP") == 0) {
        output_engine_stop_safe();
        rp_log("command=STOP output=PARKED xy=center channels=0V");
        uart_send_line("OK STOP");
        save_pending_settings();
        return;
    }

    if (strcmp(line, "CLEAR") == 0) {
        output_engine_stop_safe();
        rp_log("command=CLEAR output=PARKED ring=cleared");
        uart_send_line("OK CLEAR");
        save_pending_settings();
        return;
    }

    if (strncmp(line, "RATE ", 5) == 0) {
        uint32_t rate = (uint32_t)strtoul(&line[5], NULL, 10);
        output_engine_set_rate(rate);

        output_engine_status_t out;
        output_engine_get_status(&out);
        rp_log("command=RATE requested=%lu applied=%lu max=%lu",
               (unsigned long)rate,
               (unsigned long)out.sample_rate_hz,
               (unsigned long)out.max_rate_hz);

        /* The applied rate follows, so the ESP32 can show the real one. */
        char reply[48];
        snprintf(reply, sizeof(reply), "OK RATE %lu", (unsigned long)out.sample_rate_hz);
        uart_send_line(reply);
        return;
    }

    if (strncmp(line, "LIMIT ", 6) == 0) {
        uint32_t max_rate = output_engine_get_max_rate();

        parse_u32_arg(line, "max_rate=", &max_rate);

        output_engine_set_max_rate(max_rate);
        bool saved = save_or_defer_settings();

        output_engine_status_t out;
        output_engine_get_status(&out);
        rp_log("command=LIMIT max_rate=%lu applied_rate=%lu saved=%u",
               (unsigned long)out.max_rate_hz,
               (unsigned long)out.sample_rate_hz,
               saved ? 1u : 0u);

        char reply[48];
        snprintf(reply, sizeof(reply), "OK LIMIT %lu", (unsigned long)out.max_rate_hz);
        uart_send_line(reply);
        return;
    }

    if (strncmp(line, "SHIFT ", 6) == 0) {
        /*
           SHIFT color_us=N applies at once; while output runs it is not
           saved, because a flash write pauses this core mid-stream.
           SHIFT save writes it, and only while stopped.
        */
        output_engine_status_t out;
        const char *p = strstr(line, "color_us=");
        bool save = strstr(line, "save") != NULL;

        output_engine_get_status(&out);

        if (save && out.running) {
            uart_send_line("ERR SHIFT stop output to save");
            return;
        }

        if (p) {
            output_engine_set_color_shift((int32_t)strtol(p + 9, NULL, 10));
            s_color_shift_unsaved = true;
        }

        if (!out.running && s_color_shift_unsaved) {
            save_current_settings(true);
        }

        output_engine_get_status(&out);
        rp_log("command=SHIFT color_us=%ld samples=%lu saved=%u",
               (long)out.color_shift_us,
               (unsigned long)out.color_shift_samples,
               s_color_shift_unsaved ? 0u : 1u);

        char reply[48];
        snprintf(reply, sizeof(reply), "OK SHIFT %ld saved=%u",
                 (long)out.color_shift_us, s_color_shift_unsaved ? 0u : 1u);
        uart_send_line(reply);
        return;
    }

    if (strncmp(line, "BUFFER ", 7) == 0) {
        uint32_t size      = ring_get_capacity();
        uint32_t high_water = ring_get_high_water();
        uint32_t low_water  = ring_get_low_water();

        parse_u32_arg(line, "size=",       &size);
        parse_u32_arg(line, "high_water=", &high_water);
        parse_u32_arg(line, "low_water=",  &low_water);

        output_engine_stop_safe();
        ring_configure(size, high_water, low_water);
        save_current_settings(false);
        rp_log("command=BUFFER size=%lu high_water=%lu low_water=%lu output=PARKED",
               (unsigned long)ring_get_capacity(),
               (unsigned long)ring_get_high_water(),
               (unsigned long)ring_get_low_water());

        uart_send_line("OK BUFFER");
        return;
    }

    if (strncmp(line, "MIRROR ", 7) == 0) {
        uint32_t vx = 0;
        uint32_t vy = 0;

        parse_u32_arg(line, "x=", &vx);
        parse_u32_arg(line, "y=", &vy);

        output_engine_set_mirror(vx != 0, vy != 0);
        bool saved = save_or_defer_settings();
        rp_log("command=MIRROR x=%lu y=%lu saved=%u",
               (unsigned long)(vx != 0),
               (unsigned long)(vy != 0),
               saved ? 1u : 0u);

        uart_send_line("OK MIRROR");
        return;
    }

    uart_send_line("ERR UNKNOWN");
}

void rp_uart_poll(void)
{
    static char line[128];
    static int pos = 0;

    while (uart_is_readable(RP_UART_PORT)) {
        char c = uart_getc(RP_UART_PORT);

        if (c == '\n') {
            line[pos] = '\0';

            if (pos > 0) {
                handle_line(line);
            }

            pos = 0;
        } else if (c != '\r' && pos < (int)sizeof(line) - 1) {
            line[pos++] = c;
        }
    }
}
