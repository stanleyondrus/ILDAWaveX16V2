#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "nvs.h"

#include "app_config.h"
#include "rp_manager.h"
#include "rp_uart.h"

static const char *TAG = "rp";

#define STREAM_MAGIC0 0xA5
#define STREAM_MAGIC1 0x53
#define RP_CONFIG_CMD_TIMEOUT_MS 1500
#define RP_PAYLOAD_QUEUE_DEPTH 16
#define RP_PAYLOAD_TX_STACK 4096
#define RP_PAYLOAD_TX_PRIORITY 8
#define RP_PAYLOAD_TX_CORE 1

static SemaphoreHandle_t s_rp_mutex = NULL;
static spi_device_handle_t s_payload_spi = NULL;
static QueueHandle_t s_payload_free_q = NULL;
static QueueHandle_t s_payload_ready_q = NULL;
static TaskHandle_t s_payload_tx_task = NULL;
static volatile bool s_payload_tx_busy = false;

static TaskHandle_t s_test_stream_task = NULL;
static volatile bool s_test_stream_requested = false;
static volatile bool s_realtime_streaming = false;
static volatile rp_test_shape_t s_test_shape = RP_TEST_SHAPE_BEAM;
static volatile uint8_t s_test_color_mask = 0x07;

static uint16_t s_stream_seq = 0;
static volatile uint32_t s_stream_generation = 0;
static uint32_t s_test_phase = 0;
static int      s_output_ref = 0;   /* ref-count: START sent at 0->1, STOP at 1->0 */
static bool     s_test_holds_output = false;   /* the test stream owns one s_output_ref */

/*
   A pattern picked while one runs is switched by the generator, with a
   blanked move from its last point to the new pattern's first point.
*/
static volatile bool s_test_switch_pending = false;
static volatile rp_test_shape_t s_test_next_shape = RP_TEST_SHAPE_BEAM;
static int32_t  s_test_last_x = 0;
static int32_t  s_test_last_y = 0;
static int32_t  s_test_from_x = 0;
static int32_t  s_test_from_y = 0;
static int32_t  s_test_to_x = 0;
static int32_t  s_test_to_y = 0;
static uint32_t s_test_move_pos = 0;
static uint32_t s_test_move_len = 0;

#define TEST_SWITCH_STEP  512   /* blanked move, units per sample */
#define TEST_SWITCH_DWELL 8     /* blanked samples held at the new first point */

static rp_status_t s_status = {
    .initialized = false,
    .remote_sample_rate = 1000,
    .remote_mirror_y = true,
    .rate_limit = RP_DEFAULT_RATE_LIMIT_HZ,
    .last_error = ESP_OK,
    .remote_fw_version = "unknown",
};

static const char *RP_NVS_NAMESPACE = "rpcfg";
static const char *RP_NVS_RATE_LIMIT_KEY = "rate_limit";

static uint32_t clamp_rate_limit(uint32_t limit_hz)
{
    if (limit_hz < RP_MIN_RATE_LIMIT_HZ) {
        return RP_MIN_RATE_LIMIT_HZ;
    }

    if (limit_hz > RP_MAX_RATE_LIMIT_HZ) {
        return RP_MAX_RATE_LIMIT_HZ;
    }

    return limit_hz;
}

static void rate_limit_load(void)
{
    nvs_handle_t nvs;
    uint32_t value = 0;

    s_status.rate_limit = RP_DEFAULT_RATE_LIMIT_HZ;

    if (nvs_open(RP_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    if (nvs_get_u32(nvs, RP_NVS_RATE_LIMIT_KEY, &value) == ESP_OK) {
        s_status.rate_limit = clamp_rate_limit(value);
    }

    nvs_close(nvs);
}

static esp_err_t rate_limit_save(uint32_t limit_hz)
{
    nvs_handle_t nvs;

    esp_err_t ret = nvs_open(RP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, RP_NVS_RATE_LIMIT_KEY, limit_hz);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

typedef struct {
    uint8_t *packet;
    uint32_t generation;
} rp_payload_slot_t;

static rp_payload_slot_t s_payload_slots[RP_PAYLOAD_QUEUE_DEPTH];


static uint32_t checksum32_payload(const uint8_t *data, size_t len)
{
    uint32_t c = 0;

    for (size_t i = 0; i < len; i++) {
        c += data[i];
    }

    return c;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int parse_i32_key(const char *s, const char *key, int32_t *out)
{
    const char *p = strstr(s, key);

    if (!p) {
        return 0;
    }

    p += strlen(key);

    long v = 0;
    if (sscanf(p, "%ld", &v) == 1) {
        *out = (int32_t)v;
        return 1;
    }

    return 0;
}

static int parse_u32_key(const char *s, const char *key, uint32_t *out)
{
    const char *p = strstr(s, key);

    if (!p) {
        return 0;
    }

    p += strlen(key);

    unsigned long v = 0;
    if (sscanf(p, "%lu", &v) == 1) {
        *out = (uint32_t)v;
        return 1;
    }

    return 0;
}

static esp_err_t uart_cmd_locked_timeout(const char *cmd, char *resp, int resp_len, int timeout_ms)
{
    if (!resp || resp_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    resp[0] = '\0';

    esp_err_t ret = rp_uart_command(cmd, resp, resp_len, timeout_ms);

    s_status.last_error = ret;

    if (ret != ESP_OK) {
        s_status.error_count++;
        ESP_LOGW(TAG,
                 "UART command failed: '%s' err=%s partial='%s'",
                 cmd,
                 esp_err_to_name(ret),
                 resp);
    }

    return ret;
}

static esp_err_t uart_cmd_locked(const char *cmd, char *resp, int resp_len)
{
    return uart_cmd_locked_timeout(cmd, resp, resp_len, 200);
}

static void refresh_version_locked(void)
{
    char resp[128];

    esp_err_t ret = uart_cmd_locked("VERSION", resp, sizeof(resp));

    if (ret == ESP_OK && strncmp(resp, "VERSION ", 8) == 0) {
        strlcpy(s_status.remote_fw_version,
                resp + 8,
                sizeof(s_status.remote_fw_version));
    } else {
        strlcpy(s_status.remote_fw_version,
                "unknown",
                sizeof(s_status.remote_fw_version));

        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Bad VERSION response: '%s'", resp);
        }
    }
}

static esp_err_t push_rate_limit_locked(uint32_t limit_hz);

static esp_err_t refresh_status_locked(void)
{
    static char resp[768];   /* under s_rp_mutex; STATUS is long */

    esp_err_t ret = uart_cmd_locked("STATUS", resp, sizeof(resp));
    if (ret != ESP_OK) {
        return ret;
    }

    if (strncmp(resp, "STATUS ", 7) != 0) {
        ret = uart_cmd_locked("STATUS", resp, sizeof(resp));
        if (ret == ESP_OK && strncmp(resp, "STATUS ", 7) == 0) {
            goto parse_status;
        }

        s_status.error_count++;
        s_status.last_error = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG, "Bad STATUS response: '%s'", resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

parse_status:
    uint32_t v = 0;

    if (parse_u32_key(resp, "running=", &v)) {
        s_status.remote_running = v ? true : false;
    }

    if (parse_u32_key(resp, "rate=", &v)) {
        s_status.remote_sample_rate = v;
    }

    if (parse_u32_key(resp, "fill=", &v)) {
        s_status.remote_buffer_fill_samples = v;
    }

    if (parse_u32_key(resp, "buffer_size=", &v)) {
        s_status.remote_buffer_size_samples = v;
    }

    if (parse_u32_key(resp, "buffer_high_water=", &v)) {
        s_status.remote_buffer_high_water_samples = v;
    }

    if (parse_u32_key(resp, "buffer_low_water=", &v)) {
        s_status.remote_buffer_low_water_samples = v;
    }

    if (parse_u32_key(resp, "buffer_max=", &v)) {
        s_status.remote_buffer_max_samples = v;
    }

    if (parse_u32_key(resp, "mirror_x=", &v)) {
        s_status.remote_mirror_x = v ? true : false;
    }

    if (parse_u32_key(resp, "mirror_y=", &v)) {
        s_status.remote_mirror_y = v ? true : false;
    }

    if (parse_u32_key(resp, "underruns=", &v)) {
        s_status.remote_underruns = v;
    }

    if (parse_u32_key(resp, "payload_errors=", &v)) {
        s_status.remote_payload_errors = v;
    }

    if (parse_u32_key(resp, "stream_packets=", &v)) {
        s_status.remote_stream_packets = v;
    }

    if (parse_u32_key(resp, "bad_packets=", &v)) {
        s_status.remote_stream_bad_packets = v;
    }

    if (parse_u32_key(resp, "dropped_packets=", &v)) {
        s_status.remote_stream_dropped_packets = v;
    }

    if (parse_u32_key(resp, "rx_timeouts=", &v)) {
        s_status.remote_stream_rx_timeouts = v;
    }

    /* RP firmware 0.2.0 and later. Older firmware leaves these at 0. */
    if (parse_u32_key(resp, "max_rate=", &v)) {
        s_status.remote_max_rate = v;
    }

    if (parse_u32_key(resp, "requested_rate=", &v)) {
        s_status.remote_requested_rate = v;
    }

    if (parse_u32_key(resp, "max_lit_step=", &v)) {
        s_status.remote_max_lit_step = v;
    }

    if (parse_u32_key(resp, "lit_big_steps=", &v)) {
        s_status.remote_lit_big_steps = v;
    }

    if (parse_u32_key(resp, "blank_big_steps=", &v)) {
        s_status.remote_blank_big_steps = v;
    }

    if (parse_u32_key(resp, "stale_packets=", &v)) {
        s_status.remote_stale_packets = v;
    }

    if (parse_u32_key(resp, "seq_gaps=", &v)) {
        s_status.remote_seq_gaps = v;
    }

    if (parse_u32_key(resp, "seq_missing=", &v)) {
        s_status.remote_seq_missing = v;
    }

    if (parse_u32_key(resp, "rx_partial=", &v)) {
        s_status.remote_rx_partial = v;
    }

    int32_t sv = 0;
    if (parse_i32_key(resp, "color_shift_us=", &sv)) {
        s_status.remote_color_shift_us = sv;
    }

    if (parse_u32_key(resp, "color_shift_samples=", &v)) {
        s_status.remote_color_shift_samples = v;
    }

    /* RP firmware without the key always saved on SHIFT. */
    s_status.remote_color_shift_saved =
        !parse_u32_key(resp, "color_shift_saved=", &v) || v != 0;

    s_status.last_error = ESP_OK;

    /*
       An RP that rebooted on its own (reflash, watchdog) comes back with the
       limit from its flash, which may be older than the ESP32's. Push ours
       again whenever they disagree; the RP saves it, so this happens once.
    */
    if (s_status.remote_max_rate != 0 && s_status.remote_max_rate != s_status.rate_limit) {
        ESP_LOGW(TAG, "RP rate limit %lu differs from %lu; re-syncing",
                 (unsigned long)s_status.remote_max_rate,
                 (unsigned long)s_status.rate_limit);
        push_rate_limit_locked(s_status.rate_limit);
    }

    return ESP_OK;
}

/*
   Push the ESP32's limit to an RP that supports LIMIT, so the RP enforces it
   on its own as well. Returns ESP_ERR_NOT_SUPPORTED for older RP firmware.
*/
static esp_err_t push_rate_limit_locked(uint32_t limit_hz)
{
    char cmd[64];
    char resp[128];

    snprintf(cmd, sizeof(cmd), "LIMIT max_rate=%lu", (unsigned long)limit_hz);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    if (strncmp(resp, "OK LIMIT", 8) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t v = 0;
    if (sscanf(resp + 8, "%lu", (unsigned long *)&v) == 1) {
        s_status.remote_max_rate = v;
    } else {
        s_status.remote_max_rate = limit_hz;
    }

    return ESP_OK;
}

static int16_t clamp_i16(int32_t v)
{
    if (v < -32768) {
        return -32768;
    }

    if (v > 32767) {
        return 32767;
    }

    return (int16_t)v;
}

static uint32_t isqrt_u32(uint32_t n)
{
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;

    while (bit > n) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }

        bit >>= 2;
    }

    return root;
}

static int32_t lerp_i32(int32_t a, int32_t b, uint32_t pos, uint32_t span)
{
    if (span == 0) {
        return b;
    }

    return a + (int32_t)(((int64_t)(b - a) * (int64_t)pos) / (int64_t)span);
}

static void line_point(int32_t ax, int32_t ay,
                       int32_t bx, int32_t by,
                       uint32_t pos, uint32_t span,
                       int32_t *x, int32_t *y)
{
    *x = lerp_i32(ax, bx, pos, span);
    *y = lerp_i32(ay, by, pos, span);
}

/*
   Colour shift test frame, half scale. Three columns, red, green and blue,
   each with a pair of short white lines drawn left to right: the upper one
   ends at the column, the lower one starts there. A column does not move
   sideways with the colour shift; a line slides along its direction of
   travel, so with the colours late the upper line crosses its column and
   the lower one leaves a gap, and early the other way round. The right
   shift has both lines just touching the column. The beam runs on blanked
   through the column after the upper line, and runs up blanked into the
   lower one, at line speed, so an error shows as the line extending along
   itself and the scanners do not slow at the column.
*/
#define CS_COL_X    10000
#define CS_COL_Y    9000
#define CS_LINE     6000
#define CS_UPPER_Y  4000
#define CS_LOWER_Y  (-4000)
#define CS_RUN      1000    /* blanked run past/into the column at line speed */

typedef struct {
    int16_t ax, ay, bx, by;
    uint16_t samples;
    uint8_t color;      /* colour bits; 0 = blanked move */
} test_segment_t;

/* One column's lines and column. The blanked move in starts at the top of the
   previous column, from_x, and takes move samples. */
#define CS_COLUMN(cx, from_x, move, color)                                                  \
    { (from_x), CS_COL_Y, (cx) - CS_LINE, CS_UPPER_Y, (move), 0 },                        \
    { (cx) - CS_LINE, CS_UPPER_Y, (cx), CS_UPPER_Y, 48, 7 },                          \
    { (cx), CS_UPPER_Y, (cx) + CS_RUN, CS_UPPER_Y, 8, 0 },                            \
    { (cx) + CS_RUN, CS_UPPER_Y, (cx) - CS_RUN, CS_LOWER_Y, 32, 0 },                  \
    { (cx) - CS_RUN, CS_LOWER_Y, (cx), CS_LOWER_Y, 8, 0 },                            \
    { (cx), CS_LOWER_Y, (cx) + CS_LINE, CS_LOWER_Y, 48, 7 },                          \
    { (cx) + CS_LINE, CS_LOWER_Y, (cx), -CS_COL_Y, 32, 0 },                           \
    { (cx), -CS_COL_Y, (cx), CS_COL_Y, 104, (color) }

static const test_segment_t s_color_shift_frame[] = {
    CS_COLUMN(-CS_COL_X,  CS_COL_X, 96, 1),
    CS_COLUMN(0,         -CS_COL_X, 48, 2),
    CS_COLUMN(CS_COL_X,   0,        48, 4),
};

#define CS_SEGMENTS (sizeof(s_color_shift_frame) / sizeof(s_color_shift_frame[0]))

/* Position and colour bits of sample p of the colour shift frame. */
static uint8_t color_shift_point(uint32_t p, uint8_t color_mask, int32_t *x, int32_t *y)
{
    uint32_t total = 0;

    for (size_t i = 0; i < CS_SEGMENTS; i++) {
        total += s_color_shift_frame[i].samples;
    }

    uint32_t t = p % total;
    size_t i = 0;

    while (t >= s_color_shift_frame[i].samples) {
        t -= s_color_shift_frame[i].samples;
        i++;
    }

    const test_segment_t *seg = &s_color_shift_frame[i];

    line_point(seg->ax, seg->ay, seg->bx, seg->by, t, seg->samples - 1u, x, y);

    if (seg->color == 0) {
        return 0;
    }

    /* A single-colour laser still sees every column in its own colour. */
    uint8_t bits = seg->color & color_mask;
    return bits ? bits : color_mask;
}

/* Position and colour bits (0 = blanked) of sample p of a test shape. */
static uint8_t test_shape_point(rp_test_shape_t shape, uint32_t p, uint8_t color_mask,
                                int32_t *out_x, int32_t *out_y)
{
    int32_t x = 0;
    int32_t y = 0;
    bool blanked = false;
    uint8_t bits = color_mask;

    if (shape == RP_TEST_SHAPE_BEAM) {
        x = 0;
        y = 0;
    } else if (shape == RP_TEST_SHAPE_COLOR_SHIFT) {
        bits = color_shift_point(p, color_mask, &x, &y);
        blanked = bits == 0;
    } else if (shape == RP_TEST_SHAPE_CIRCLE) {
        const int32_t r = 32767;
        const uint32_t rr = (uint32_t)(r * r);
        uint32_t t = p % 1024;
        uint32_t q = t & 255u;
        int32_t v = r - (int32_t)((q * (uint32_t)r) / 255u);
        int32_t h = (int32_t)isqrt_u32(rr - (uint32_t)(v * v));

        if (t < 256u) {
            x = v;
            y = h;
        } else if (t < 512u) {
            x = -h;
            y = v;
        } else if (t < 768u) {
            x = -v;
            y = -h;
        } else {
            x = h;
            y = -v;
        }
    } else if (shape == RP_TEST_SHAPE_CROSS) {
        uint32_t t = p % 1024;

        if (t < 448u) {
            line_point(-32768, 0, 32767, 0, t, 447u, &x, &y);
        } else if (t < 512u) {
            blanked = true;
            line_point(32767, 0, 0, -32768, t - 448u, 63u, &x, &y);
        } else if (t < 960u) {
            line_point(0, -32768, 0, 32767, t - 512u, 447u, &x, &y);
        } else {
            blanked = true;
            line_point(0, 32767, -32768, 0, t - 960u, 63u, &x, &y);
        }
    } else if (shape == RP_TEST_SHAPE_HEART) {
        uint32_t t = p % 1536;
        uint32_t seg = t / 256u;
        uint32_t q = t & 255u;

        switch (seg) {
        case 0:
            line_point(0, 12000, -21000, 30000, q, 255u, &x, &y);
            break;
        case 1:
            line_point(-21000, 30000, -32768, 5000, q, 255u, &x, &y);
            break;
        case 2:
            line_point(-32768, 5000, 0, -32768, q, 255u, &x, &y);
            break;
        case 3:
            line_point(0, -32768, 32767, 5000, q, 255u, &x, &y);
            break;
        case 4:
            line_point(32767, 5000, 21000, 30000, q, 255u, &x, &y);
            break;
        default:
            line_point(21000, 30000, 0, 12000, q, 255u, &x, &y);
            break;
        }
    } else {
        uint32_t t = p % 1280;

        if (t < 256u) {
            line_point(-32768, -32768, 32767, -32768, t, 255u, &x, &y);
        } else if (t < 512u) {
            line_point(32767, -32768, 32767, 32767, t - 256u, 255u, &x, &y);
        } else if (t < 768u) {
            line_point(32767, 32767, -32768, 32767, t - 512u, 255u, &x, &y);
        } else if (t < 1024u) {
            line_point(-32768, 32767, -32768, -32768, t - 768u, 255u, &x, &y);
        } else {
            blanked = true;
            line_point(-32768, -32768, -32768, -32768, t - 1024u, 255u, &x, &y);
        }
    }

    *out_x = x;
    *out_y = y;
    return blanked ? 0 : bits;
}

/* Start the blanked move to `shape`, from the last point sent. */
static void test_switch_begin(rp_test_shape_t shape, uint8_t color_mask)
{
    int32_t dx;
    int32_t dy;

    s_test_shape = shape;
    s_test_phase = 0;
    test_shape_point(shape, 0, color_mask, &s_test_to_x, &s_test_to_y);
    s_test_from_x = s_test_last_x;
    s_test_from_y = s_test_last_y;

    dx = s_test_to_x - s_test_from_x;
    dy = s_test_to_y - s_test_from_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    s_test_move_len = (uint32_t)((dx > dy ? dx : dy) / TEST_SWITCH_STEP) + 1u;
    s_test_move_pos = 0;
}

static void build_test_samples(int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    const uint8_t color_mask = s_test_color_mask & 0x07;

    for (int s = 0; s < RP_STREAM_SAMPLES_PER_PACKET; s++) {
        int32_t x = 0;
        int32_t y = 0;
        uint8_t bits = 0;

        if (s_test_switch_pending) {
            s_test_switch_pending = false;
            test_switch_begin(s_test_next_shape, color_mask);
        }

        if (s_test_move_pos < s_test_move_len + TEST_SWITCH_DWELL) {
            uint32_t pos = s_test_move_pos < s_test_move_len ? s_test_move_pos : s_test_move_len;
            line_point(s_test_from_x, s_test_from_y, s_test_to_x, s_test_to_y,
                       pos, s_test_move_len, &x, &y);
            s_test_move_pos++;
        } else {
            bits = test_shape_point(s_test_shape, s_test_phase, color_mask, &x, &y);
            s_test_phase++;
        }

        s_test_last_x = x;
        s_test_last_y = y;

        samples[s][0] = clamp_i16(x);
        samples[s][1] = clamp_i16(y);
        samples[s][2] = (bits & 0x01) ? 32767 : -32768;
        samples[s][3] = (bits & 0x02) ? 32767 : -32768;
        samples[s][4] = (bits & 0x04) ? 32767 : -32768;
        samples[s][5] = bits ? 32767 : -32768;
        for (int ch = 6; ch < RP_SAMPLE_CHANNELS; ch++) {
            samples[s][ch] = 0;
        }
    }
}

/*
   Falling edges on READY, counted in hardware-latched GPIO interrupts. The
   RP drops READY once a packet is in and raises it again as soon as it is
   ready for the next, so the low time can be shorter than this task is held
   off by others; polling the level then missed it and stalled 100 ms.
*/
static volatile uint32_t s_ready_falls = 0;

static void IRAM_ATTR ready_fall_isr(void *arg)
{
    (void)arg;
    s_ready_falls++;
}

static esp_err_t payload_spi_start(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RP_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = RP_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RP_STREAM_PACKET_BYTES,
    };

    esp_err_t ret = spi_bus_initialize(RP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    gpio_config_t ready_io = {
        .pin_bit_mask = 1ULL << RP_MISO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ret = gpio_config(&ready_io);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_isr_handler_add(RP_MISO_GPIO, ready_fall_isr, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = RP_SPI_PAYLOAD_SPEED_HZ,
        .mode = 3,
        .spics_io_num = RP_CS_GPIO,
        .queue_size = 4,
        .cs_ena_pretrans = 16,
        .cs_ena_posttrans = 16,
    };

    ret = spi_bus_add_device(RP_SPI_HOST, &dev_cfg, &s_payload_spi);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "Payload SPI ready speed=%d packet=%d bytes ready_gpio=%d",
             RP_SPI_PAYLOAD_SPEED_HZ,
             RP_STREAM_PACKET_BYTES,
             RP_MISO_GPIO);

    return ESP_OK;
}

static esp_err_t wait_rp_ready_high(uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;

    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level(RP_MISO_GPIO)) {
            esp_rom_delay_us(3);
            return ESP_OK;
        }

        for (int i = 0; i < 100; i++) {
            esp_rom_delay_us(2);
            if (gpio_get_level(RP_MISO_GPIO)) {
                esp_rom_delay_us(3);
                return ESP_OK;
            }
        }

        taskYIELD();
    }

    return ESP_ERR_TIMEOUT;
}

/* Wait for READY to fall after `falls` edges had been counted. */
static esp_err_t wait_rp_ready_fall(uint32_t falls, uint32_t timeout_us)
{
    int64_t deadline = esp_timer_get_time() + timeout_us;

    while (esp_timer_get_time() < deadline) {
        if (s_ready_falls != falls) {
            return ESP_OK;
        }

        for (int i = 0; i < 100; i++) {
            esp_rom_delay_us(2);
            if (s_ready_falls != falls) {
                return ESP_OK;
            }
        }

        taskYIELD();
    }

    return ESP_ERR_TIMEOUT;
}

static void build_packet_payload(uint8_t *packet, const uint8_t *payload)
{
    packet[0] = STREAM_MAGIC0;
    packet[1] = STREAM_MAGIC1;

    uint16_t seq = __atomic_fetch_add(&s_stream_seq, 1, __ATOMIC_RELAXED);
    put_u16_le(&packet[2], seq);
    put_u16_le(&packet[4], RP_STREAM_SAMPLES_PER_PACKET);
    put_u16_le(&packet[6], 0);

    memcpy(&packet[RP_STREAM_HEADER_BYTES], payload, RP_STREAM_PAYLOAD_BYTES);

    uint32_t checksum =
        checksum32_payload(&packet[RP_STREAM_HEADER_BYTES], RP_STREAM_PAYLOAD_BYTES);

    put_u32_le(&packet[8], checksum);
}

static esp_err_t transmit_built_packet(const uint8_t *packet)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));

    t.length = RP_STREAM_PACKET_BYTES * 8;
    t.tx_buffer = packet;

    /* READY is high here; its next fall says the RP has the whole packet. */
    uint32_t falls = s_ready_falls;

    esp_err_t ret = spi_device_transmit(s_payload_spi, &t);
    if (ret != ESP_OK) {
        s_status.payload_spi_errors++;
        return ret;
    }

    ret = wait_rp_ready_fall(falls, 100000);
    if (ret != ESP_OK) {
        s_status.payload_ack_timeouts++;
    }

    return ret;
}

static void payload_record_tx_result(esp_err_t ret)
{
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        s_status.error_count++;
        return;
    }

    s_status.test_stream_blocks_sent++;
    s_status.last_error = ESP_OK;
}

static void payload_tx_task(void *arg)
{
    (void)arg;

    while (true) {
        rp_payload_slot_t *slot = NULL;

        if (xQueueReceive(s_payload_ready_q, &slot, portMAX_DELAY) != pdTRUE || !slot) {
            continue;
        }

        uint32_t generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);
        if (slot->generation == generation) {
            s_payload_tx_busy = true;
            esp_err_t ret = wait_rp_ready_high(500000);
            if (ret != ESP_OK) {
                s_status.payload_ready_timeouts++;
            }

            generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);
            if (ret == ESP_OK && slot->generation == generation) {
                ret = transmit_built_packet(slot->packet);
            }

            payload_record_tx_result(ret);
            s_payload_tx_busy = false;
        }

        xQueueSend(s_payload_free_q, &slot, portMAX_DELAY);
    }
}

static void payload_queue_reset(void)
{
    __atomic_add_fetch(&s_stream_generation, 1, __ATOMIC_ACQ_REL);

    if (!s_payload_ready_q || !s_payload_free_q) {
        return;
    }

    rp_payload_slot_t *slot = NULL;
    while (xQueueReceive(s_payload_ready_q, &slot, 0) == pdTRUE) {
        if (slot) {
            xQueueSend(s_payload_free_q, &slot, 0);
        }
    }
}

static esp_err_t payload_queue_start(void)
{
    if (s_payload_tx_task) {
        return ESP_OK;
    }

    if (!s_payload_free_q) {
        s_payload_free_q = xQueueCreate(RP_PAYLOAD_QUEUE_DEPTH, sizeof(rp_payload_slot_t *));
        if (!s_payload_free_q) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_payload_ready_q) {
        s_payload_ready_q = xQueueCreate(RP_PAYLOAD_QUEUE_DEPTH, sizeof(rp_payload_slot_t *));
        if (!s_payload_ready_q) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < RP_PAYLOAD_QUEUE_DEPTH; i++) {
        if (!s_payload_slots[i].packet) {
            s_payload_slots[i].packet = heap_caps_malloc(
                RP_STREAM_PACKET_BYTES,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

            if (!s_payload_slots[i].packet) {
                return ESP_ERR_NO_MEM;
            }
        }

        rp_payload_slot_t *slot = &s_payload_slots[i];
        xQueueSend(s_payload_free_q, &slot, 0);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        payload_tx_task,
        "rp_tx",
        RP_PAYLOAD_TX_STACK,
        NULL,
        RP_PAYLOAD_TX_PRIORITY,
        &s_payload_tx_task,
        RP_PAYLOAD_TX_CORE);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t rp_manager_wait_payload_idle(uint32_t timeout_ms)
{
    if (!s_payload_free_q || !s_payload_ready_q) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline_us) {
        UBaseType_t ready = uxQueueMessagesWaiting(s_payload_ready_q);
        UBaseType_t free_count = uxQueueMessagesWaiting(s_payload_free_q);

        if (ready == 0 && !s_payload_tx_busy && free_count >= RP_PAYLOAD_QUEUE_DEPTH) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_packet_payload(const uint8_t *payload)
{
    if (!s_payload_spi || !s_payload_free_q || !s_payload_ready_q) {
        return ESP_ERR_INVALID_STATE;
    }

    rp_payload_slot_t *slot = NULL;
    if (xQueueReceive(s_payload_free_q, &slot, portMAX_DELAY) != pdTRUE || !slot) {
        return ESP_FAIL;
    }

    build_packet_payload(slot->packet, payload);
    slot->generation = __atomic_load_n(&s_stream_generation, __ATOMIC_ACQUIRE);

    if (xQueueSend(s_payload_ready_q, &slot, portMAX_DELAY) != pdTRUE) {
        xQueueSend(s_payload_free_q, &slot, portMAX_DELAY);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t send_stream_packet_spi(void)
{
    static int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];

    build_test_samples(samples);

    return send_packet_payload((const uint8_t *)samples);
}

esp_err_t rp_manager_send_samples_packet(
    const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    return send_packet_payload((const uint8_t *)samples);
}

static void test_stream_task(void *arg)
{
    (void)arg;

    int64_t next_status_us = 0;

    while (s_test_stream_requested) {
        esp_err_t ret = send_stream_packet_spi();

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "stream packet send failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        int64_t now = esp_timer_get_time();

        if (now >= next_status_us) {
            if (s_rp_mutex &&
                xSemaphoreTake(s_rp_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                refresh_status_locked();
                xSemaphoreGive(s_rp_mutex);
            }

            next_status_us = now + 250000;
        }

        taskYIELD();
    }

    s_status.test_stream_running = false;
    s_test_stream_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rp_manager_start(void)
{
    if (!s_rp_mutex) {
        s_rp_mutex = xSemaphoreCreateMutex();

        if (!s_rp_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = rp_uart_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    ret = payload_spi_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    ret = payload_queue_start();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    char resp[128];

    s_output_ref = 0;
    s_test_holds_output = false;
    rate_limit_load();

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    ret = uart_cmd_locked("PING", resp, sizeof(resp));
    if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
        s_status.initialized = true;
        s_status.ping_count++;

        refresh_version_locked();

        ret = refresh_status_locked();

        /* The ESP32's saved limit is the one the user set; keep the RP in step. */
        if (ret == ESP_OK && s_status.remote_max_rate != 0 &&
            s_status.remote_max_rate != s_status.rate_limit) {
            esp_err_t lim_ret = push_rate_limit_locked(s_status.rate_limit);
            ESP_LOGI(TAG, "RP rate limit %lu -> %lu: %s",
                     (unsigned long)s_status.remote_max_rate,
                     (unsigned long)s_status.rate_limit,
                     esp_err_to_name(lim_ret));
        }
        ESP_LOGI(TAG, "Scanner rate limit %lu pps (RP limit %lu)",
                 (unsigned long)s_status.rate_limit,
                 (unsigned long)s_status.remote_max_rate);

        /* The RP keeps its last rate across an ESP32 reboot; pull it under the limit. */
        if (ret == ESP_OK && s_status.remote_sample_rate > s_status.rate_limit) {
            char rate_cmd[64];
            snprintf(rate_cmd, sizeof(rate_cmd), "RATE %lu", (unsigned long)s_status.rate_limit);
            if (uart_cmd_locked(rate_cmd, resp, sizeof(resp)) == ESP_OK) {
                ESP_LOGW(TAG, "RP rate %lu above limit at boot; set to %lu",
                         (unsigned long)s_status.remote_sample_rate,
                         (unsigned long)s_status.rate_limit);
                s_status.remote_sample_rate = s_status.rate_limit;
            }
        }
    } else {
        ESP_LOGW(TAG, "RP not ready or bad ping response: '%s'", resp);
        s_status.initialized = false;
        s_status.last_error = ESP_ERR_INVALID_RESPONSE;
        s_status.error_count++;
        strlcpy(s_status.remote_fw_version,
                "unknown",
                sizeof(s_status.remote_fw_version));

        ret = ESP_OK;
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_ping(void)
{
    if (s_realtime_streaming) {
        return ESP_OK;
    }

    char resp[128];

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked("PING", resp, sizeof(resp));
    if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
        s_status.initialized = true;
        s_status.ping_count++;

        refresh_version_locked();
    } else if (ret == ESP_OK) {
        ret = uart_cmd_locked("PING", resp, sizeof(resp));
        if (ret == ESP_OK && strcmp(resp, "PONG") == 0) {
            s_status.initialized = true;
            s_status.ping_count++;
            refresh_version_locked();
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_RESPONSE;
            s_status.last_error = ret;
            s_status.error_count++;
            ESP_LOGW(TAG, "Bad PING response: '%s'", resp);
        }
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_get_remote_status(void)
{
    if (s_realtime_streaming) {
        return ESP_OK;
    }

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);
    esp_err_t ret = refresh_status_locked();
    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_output_start(void)
{
    char resp[128];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    if (s_output_ref++ == 0) {
        ret = uart_cmd_locked("START", resp, sizeof(resp));
        if (ret == ESP_OK)
            s_status.remote_running = true;
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

/*
   A packet already on the wire still lands after a STOP or CLEAR sent now,
   and the next START would play it: let it finish first. Waits in whole
   ticks, since pdMS_TO_TICKS(1) is 0 at 100 Hz; the TX task gives up on
   READY after 500 ms.
*/
static void wait_payload_tx_idle(void)
{
    int64_t deadline_us = esp_timer_get_time() + 600000;

    while (s_payload_tx_busy && esp_timer_get_time() < deadline_us) {
        vTaskDelay(1);
    }
}

esp_err_t rp_manager_output_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    char resp[128];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    if (s_output_ref > 0 && --s_output_ref == 0) {
        payload_queue_reset();
        wait_payload_tx_idle();
        ret = uart_cmd_locked("STOP", resp, sizeof(resp));
        if (ret == ESP_OK)
            refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_output_force_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    char resp[128];

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    s_output_ref = 0;
    s_test_holds_output = false;
    wait_payload_tx_idle();
    esp_err_t ret = uart_cmd_locked("STOP", resp, sizeof(resp));
    if (ret == ESP_OK) {
        s_status.remote_running = false;
        refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

/*
   Drop samples a producer queued but never STARTed, so the next START does
   not play them. Does nothing while another producer holds the output,
   since its samples share the same queue.
*/
esp_err_t rp_manager_output_discard_unstarted(void)
{
    char resp[128];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    if (s_output_ref == 0) {
        payload_queue_reset();
        wait_payload_tx_idle();

        ret = uart_cmd_locked("CLEAR", resp, sizeof(resp));
        if (ret == ESP_OK)
            refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_clear_buffer(void)
{
    char resp[128];

    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    wait_payload_tx_idle();
    esp_err_t ret = uart_cmd_locked("CLEAR", resp, sizeof(resp));
    if (ret == ESP_OK) {
        ret = refresh_status_locked();
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

/* The rate the producer last asked for, before the limit; 0 = none yet. */
static uint32_t s_wanted_rate = 0;

esp_err_t rp_manager_set_rate_applied(uint32_t rate_hz, uint32_t *applied)
{
    char cmd[64];
    char resp[128];

    if (rate_hz < 1) {
        rate_hz = 1;
    }

    s_wanted_rate = rate_hz;

    if (rate_hz > s_status.rate_limit) {
        ESP_LOGW(TAG, "Rate %lu above scanner limit %lu; clamped",
                 (unsigned long)rate_hz, (unsigned long)s_status.rate_limit);
        rate_hz = s_status.rate_limit;
    }

    if (rate_hz > RP_MAX_RATE_LIMIT_HZ) {
        rate_hz = RP_MAX_RATE_LIMIT_HZ;
    }

    snprintf(cmd, sizeof(cmd), "RATE %lu", (unsigned long)rate_hz);

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked(cmd, resp, sizeof(resp));
    if (ret == ESP_OK) {
        /* RP 0.2.0+ answers "OK RATE <applied>"; older firmware just "OK RATE". */
        uint32_t v = 0;
        if (strncmp(resp, "OK RATE ", 8) == 0 && sscanf(resp + 8, "%lu", (unsigned long *)&v) == 1 && v > 0) {
            rate_hz = v;
        }
        s_status.remote_sample_rate = rate_hz;
        s_status.remote_requested_rate = rate_hz;
    }

    xSemaphoreGive(s_rp_mutex);

    if (applied) {
        *applied = s_status.remote_sample_rate;
    }

    return ret;
}

esp_err_t rp_manager_set_rate(uint32_t rate_hz)
{
    return rp_manager_set_rate_applied(rate_hz, NULL);
}

uint32_t rp_manager_get_rate_limit(void)
{
    return s_status.rate_limit;
}

/* Send one SHIFT command and record the RP's "OK SHIFT <us> saved=<0|1>". */
static esp_err_t shift_cmd(const char *cmd)
{
    char resp[128];

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret == ESP_OK && strncmp(resp, "OK SHIFT", 8) == 0) {
        long v = 0;
        uint32_t saved = 1;
        if (sscanf(resp + 8, "%ld", &v) == 1) {
            s_status.remote_color_shift_us = (int32_t)v;
        }
        parse_u32_key(resp, "saved=", &saved);
        s_status.remote_color_shift_saved = saved != 0;
    } else if (ret == ESP_OK && strncmp(resp, "ERR SHIFT", 9) == 0) {
        ret = ESP_ERR_INVALID_STATE;   /* save refused while output runs */
        ESP_LOGW(TAG, "%s: '%s'", cmd, resp);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;   /* RP firmware without SHIFT */
        ESP_LOGW(TAG, "SHIFT not supported by RP: '%s'", resp);
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_set_color_shift(int32_t shift_us)
{
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "SHIFT color_us=%ld", (long)shift_us);
    return shift_cmd(cmd);
}

esp_err_t rp_manager_save_color_shift(void)
{
    return shift_cmd("SHIFT save");
}

esp_err_t rp_manager_set_rate_limit(uint32_t limit_hz)
{
    limit_hz = clamp_rate_limit(limit_hz);

    esp_err_t ret = rate_limit_save(limit_hz);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Rate limit not saved: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);
    s_status.rate_limit = limit_hz;
    esp_err_t push_ret = push_rate_limit_locked(limit_hz);
    xSemaphoreGive(s_rp_mutex);

    ESP_LOGI(TAG, "Scanner rate limit set to %lu pps (RP: %s)",
             (unsigned long)limit_hz, esp_err_to_name(push_ret));

    /*
       Re-apply the producer's rate under the new limit, so a lower limit
       takes effect now and a higher one lets a clamped stream speed back up.
       Protocols read the running rate back from here.
    */
    esp_err_t rate_ret = ESP_OK;
    uint32_t wanted = s_wanted_rate ? s_wanted_rate : s_status.remote_sample_rate;
    uint32_t target = wanted < limit_hz ? wanted : limit_hz;
    if (target != s_status.remote_sample_rate) {
        rate_ret = rp_manager_set_rate_applied(wanted, NULL);
    }

    /* Saved here, but an RP that did not take it is still running the old limit. */
    if (push_ret != ESP_OK && push_ret != ESP_ERR_NOT_SUPPORTED) {
        return push_ret;
    }

    return rate_ret;
}

uint32_t rp_manager_get_sample_rate(void)
{
    return s_status.remote_sample_rate;
}

esp_err_t rp_manager_set_buffer_config(uint32_t size_samples,
                                       uint32_t high_water_samples,
                                       uint32_t low_water_samples)
{
    char cmd[96];
    char resp[128];

    if (size_samples < 512) {
        size_samples = 512;
    }

    if (high_water_samples < 256) {
        high_water_samples = 256;
    }

    if (high_water_samples >= size_samples) {
        high_water_samples = size_samples > 256 ? size_samples - 256 : 256;
    }

    if (low_water_samples > high_water_samples - 256) {
        low_water_samples = high_water_samples > 256 ? high_water_samples - 256 : 0;
    }

    snprintf(cmd, sizeof(cmd),
             "BUFFER size=%lu high_water=%lu low_water=%lu",
             (unsigned long)size_samples,
             (unsigned long)high_water_samples,
             (unsigned long)low_water_samples);

    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    payload_queue_reset();

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret == ESP_OK && strncmp(resp, "OK BUFFER", 9) == 0) {
        s_status.remote_buffer_size_samples = size_samples;
        s_status.remote_buffer_high_water_samples = high_water_samples;
        s_status.remote_buffer_low_water_samples = low_water_samples;
        s_status.remote_running = false;
        s_output_ref = 0;
        s_test_holds_output = false;
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
        s_status.last_error = ret;
        s_status.error_count++;
        ESP_LOGW(TAG, "Bad BUFFER response: '%s'", resp);
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

esp_err_t rp_manager_set_mirror(bool mirror_x, bool mirror_y)
{
    char cmd[64];
    char resp[128];

    snprintf(cmd, sizeof(cmd),
             "MIRROR x=%lu y=%lu",
             mirror_x ? 1ul : 0ul,
             mirror_y ? 1ul : 0ul);

    xSemaphoreTake(s_rp_mutex, portMAX_DELAY);

    esp_err_t ret = uart_cmd_locked_timeout(cmd, resp, sizeof(resp), RP_CONFIG_CMD_TIMEOUT_MS);
    if (ret == ESP_OK && strncmp(resp, "OK MIRROR", 9) == 0) {
        s_status.remote_mirror_x = mirror_x;
        s_status.remote_mirror_y = mirror_y;
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
        s_status.last_error = ret;
        s_status.error_count++;
        ESP_LOGW(TAG, "Bad MIRROR response: '%s'", resp);
    }

    xSemaphoreGive(s_rp_mutex);

    return ret;
}

void rp_manager_set_realtime_streaming(bool enabled)
{
    s_realtime_streaming = enabled;
}

esp_err_t rp_manager_send_test_block(void)
{
    return send_stream_packet_spi();
}

esp_err_t rp_manager_start_test_stream(void)
{
    return rp_manager_start_test_shape(RP_TEST_SHAPE_SQUARE);
}

esp_err_t rp_manager_start_test_shape(rp_test_shape_t shape)
{
    if (s_test_stream_task) {
        /* Running: the generator moves to the new shape blanked. */
        s_test_next_shape = shape;
        s_test_switch_pending = true;
        s_test_stream_requested = true;
        s_status.test_stream_running = true;
        return ESP_OK;
    }

    /* Fresh start: START's own blanked ramp reaches the first point. */
    s_test_switch_pending = false;
    s_test_shape = shape;
    s_test_phase = 0;
    s_test_move_len = 0;
    s_test_move_pos = TEST_SWITCH_DWELL;
    s_test_stream_requested = true;
    s_status.test_stream_running = true;

    BaseType_t ok = xTaskCreate(
        test_stream_task,
        "rp_stream",
        6144,
        NULL,
        6,
        &s_test_stream_task
    );

    if (ok != pdPASS) {
        s_test_stream_requested = false;
        s_status.test_stream_running = false;
        s_test_stream_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Held until rp_manager_stop_test_stream(), even if START failed. */
    s_test_holds_output = true;
    esp_err_t ret = rp_manager_output_start();
    ESP_LOGI(TAG, "test stream start: shape %d, output ref %d, START %s",
             (int)shape, s_output_ref, esp_err_to_name(ret));
    return ret;
}

void rp_manager_set_test_color_mask(uint8_t mask)
{
    s_test_color_mask = mask & 0x07;
}

esp_err_t rp_manager_stop_test_stream(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    uint32_t waited_ms = 0;
    while (s_test_stream_task && waited_ms < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    if (s_test_stream_task) {
        ESP_LOGW(TAG, "test stream stop: task still running after %lu ms", (unsigned long)waited_ms);
        return ESP_ERR_TIMEOUT;
    }

    /*
       Give back the output the pattern took, so the RP stops (parked,
       blanked) unless another producer still holds it.
    */
    if (s_test_holds_output) {
        s_test_holds_output = false;
        int ref_before = s_output_ref;
        esp_err_t ret = rp_manager_output_stop();
        ESP_LOGI(TAG, "test stream stop: waited %lu ms, output ref %d -> %d, STOP %s",
                 (unsigned long)waited_ms, ref_before, s_output_ref, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "test stream stop: waited %lu ms, no output held (ref %d)",
             (unsigned long)waited_ms, s_output_ref);
    return ESP_OK;
}

void rp_manager_get_status(rp_status_t *status)
{
    if (!status) {
        return;
    }

    if (!s_rp_mutex) {
        *status = s_status;
        return;
    }

    if (s_realtime_streaming) {
        if (xSemaphoreTake(s_rp_mutex, 0) == pdTRUE) {
            *status = s_status;
            xSemaphoreGive(s_rp_mutex);
        } else {
            *status = s_status;
        }

        return;
    }

    if (xSemaphoreTake(s_rp_mutex, 0) == pdTRUE) {
        *status = s_status;
        xSemaphoreGive(s_rp_mutex);
    } else {
        *status = s_status;
    }
}
