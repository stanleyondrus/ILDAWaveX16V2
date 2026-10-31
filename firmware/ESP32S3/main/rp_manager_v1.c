/*
 * rp_manager_v1.c - DAC80508 direct output for board V1 (ESP32-S3 only).
 * Implements the same rp_manager API used by all protocol feed tasks.
 *
 * Hardware:
 *   SPI3:  CS=DAC_CS_GPIO, SCLK=DAC_SCLK_GPIO, MOSI=DAC_MOSI_GPIO
 *   GPIO:  DAC_SHUTTER_GPIO - active-high shutter control
 *
 * DAC80508 init: TRIGGER reset, GAIN=0x00FF, SYNC=0xFFFF (all channels synchronous)
 * SPI mode 1 (CPOL=0, CPHA=1), 24-bit frames, MSB first.
 *
 * Channel layout (matches RP2350 firmware):
 *   sample[0]=X  -> DAC register 0x0E
 *   sample[1]=Y  -> DAC register 0x0F
 *   sample[2]=R  -> DAC register 0x0D
 *   sample[3]=G  -> DAC register 0x0C
 *   sample[4]=B  -> DAC register 0x0B
 *   sample[5]=I  -> DAC register 0x0A
 *   sample[6]=U1 -> DAC register 0x09  (auxiliary)
 *   sample[7]=U2 -> DAC register 0x08  (auxiliary)
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "hal/spi_ll.h"

#include "nvs.h"

#include "app_config.h"
#include "rp_manager.h"

#if BOARD_VERSION != 1
#error "rp_manager_v1.c must only be compiled for BOARD_VERSION=1"
#endif

static const char *TAG = "dac";

/* DAC80508 register map */
#define DAC_REG_SYNC     0x02
#define DAC_REG_GAIN     0x04
#define DAC_REG_TRIGGER  0x05
#define DAC_REG_DAC_BASE 0x08

/* Physical DAC channel numbers — must match dac80508.c in the RP2350 firmware */
#define DAC_CH_X  6
#define DAC_CH_Y  7
#define DAC_CH_R  5
#define DAC_CH_G  4
#define DAC_CH_B  3
#define DAC_CH_I  2
#define DAC_CH_U1 1   /* auxiliary — written per-sample to match RP2350 behaviour */
#define DAC_CH_U2 0

/* Internal ring buffer: power-of-2 for fast masking */
#define V1_RING_SIZE  4096
#define V1_RING_MASK  (V1_RING_SIZE - 1)
#define V1_OUTPUT_TASK_STACK 8192
#define V1_TEST_TASK_STACK 6144
#define V1_STOP_WAIT_MS 500
#define V1_DAC_SPI_CLOCK_HZ 50000000
#define V1_OUTPUT_RESYNC_PERIODS 2

static int16_t s_ring[V1_RING_SIZE][RP_SAMPLE_CHANNELS];
static volatile uint32_t s_ring_head = 0;
static volatile uint32_t s_ring_tail = 0;

static SemaphoreHandle_t s_mutex       = NULL;
static SemaphoreHandle_t s_push_mutex  = NULL; /* serialises multi-producer push */
static spi_device_handle_t s_dac_spi  = NULL;
static spi_dev_t          *s_dac_hw   = NULL;  /* direct HW handle for hot-path LL writes */
static TaskHandle_t s_output_task_handle = NULL;
static volatile bool s_output_running    = false;

static TaskHandle_t s_test_task_handle      = NULL;
static volatile bool s_test_stream_requested = false;
static volatile rp_test_shape_t s_test_shape = RP_TEST_SHAPE_BEAM;
static volatile uint8_t s_test_color_mask    = 0x07;
static uint32_t s_test_phase = 0;

static int s_output_ref = 0;

static rp_status_t s_status = {
    .initialized            = false,
    .remote_sample_rate     = 30000,
    .remote_mirror_y        = true,
    .remote_buffer_size_samples = V1_RING_SIZE,
    .rate_limit             = RP_DEFAULT_RATE_LIMIT_HZ,
    .remote_max_rate        = RP_DEFAULT_RATE_LIMIT_HZ,
    .remote_color_shift_saved = true,
    .last_error             = ESP_OK,
    .remote_fw_version      = "V1-ESP32S3",
};

/* V1 output tops out here whatever the limit says. */
#define V1_MAX_RATE_HZ 100000u

/* Same NVS keys as V2, so a saved scanner limit means the same thing on both. */
static const char *V1_NVS_NAMESPACE = "rpcfg";
static const char *V1_NVS_RATE_LIMIT_KEY = "rate_limit";

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

    if (nvs_open(V1_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u32(nvs, V1_NVS_RATE_LIMIT_KEY, &value) == ESP_OK) {
            s_status.rate_limit = clamp_rate_limit(value);
        }
        nvs_close(nvs);
    }

    /* On V1 the ESP32-S3 is the DAC engine, so its limit is the engine's. */
    s_status.remote_max_rate = s_status.rate_limit;
}

static esp_err_t rate_limit_save(uint32_t limit_hz)
{
    nvs_handle_t nvs;

    esp_err_t ret = nvs_open(V1_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, V1_NVS_RATE_LIMIT_KEY, limit_hz);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static inline TickType_t delay_ticks_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

/* ------------------------------------------------------------------ */
/* Ring buffer helpers (SPSC - one writer at a time via push_mutex)   */
/* ------------------------------------------------------------------ */

static inline uint32_t ring_fill(void)
{
    uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE);
    return (head - tail) & V1_RING_MASK;
}

static inline uint32_t ring_space(void)
{
    return V1_RING_SIZE - 1 - ring_fill();
}

static bool ring_pop(int16_t out[RP_SAMPLE_CHANNELS])
{
    uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);

    uint32_t tail = __atomic_load_n(&s_ring_tail, __ATOMIC_RELAXED);

    if (tail == head) {
        return false;
    }

    memcpy(out, s_ring[tail], RP_SAMPLE_CHANNELS * sizeof(int16_t));
    __atomic_store_n(&s_ring_tail, (tail + 1) & V1_RING_MASK, __ATOMIC_RELEASE);
    return true;
}

static void ring_clear(void)
{
    __atomic_store_n(&s_ring_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ring_tail, 0, __ATOMIC_RELEASE);
    s_status.remote_buffer_fill_samples = 0;
}

static esp_err_t wait_output_task_stopped(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (s_output_task_handle && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    return s_output_task_handle ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t suspend_core1_idle_wdt(void) { return ESP_OK; }
static void resume_core1_idle_wdt(void) {}

/* Push one full packet (256 samples). Blocks up to timeout_ms for space. */
static esp_err_t ring_push_packet(
    const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS],
    uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (ring_space() < RP_STREAM_SAMPLES_PER_PACKET) {
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        /* output_task drains the ring on core 1 independently.
         * taskYIELD lets same-priority core-0 tasks run; higher-priority
         * tasks preempt us regardless.  A 1 ms sleep here would add
         * quantisation overhead (~0.5 ms per push at 30 kpps) that reduces
         * the effective push rate below the output rate. */
        taskYIELD();
    }

    uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_RELAXED);

    for (int i = 0; i < RP_STREAM_SAMPLES_PER_PACKET; i++) {
        memcpy(s_ring[head], samples[i], RP_SAMPLE_CHANNELS * sizeof(int16_t));
        head = (head + 1) & V1_RING_MASK;
    }

    __atomic_store_n(&s_ring_head, head, __ATOMIC_RELEASE);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* SPI / DAC low-level                                                 */
/* ------------------------------------------------------------------ */

static inline uint16_t signed_to_dac(int16_t v)
{
    return (uint16_t)((int32_t)v + 32768);
}

static inline void dac_fill_frame(uint8_t tx[4], uint8_t reg, uint16_t value)
{
    tx[0] = reg;
    tx[1] = (uint8_t)(value >> 8);
    tx[2] = (uint8_t)(value & 0xFF);
    tx[3] = 0;
}

/* Driver-path write — used for init and shutdown only.
 * spi_device_polling_transmit has ~28 µs overhead per call; never use in the
 * hot output loop. */
static void dac_write(uint8_t reg, uint16_t value)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 24,
    };
    dac_fill_frame(t.tx_data, reg, value);
    spi_device_polling_transmit(s_dac_spi, &t);
}

/* Direct LL write — bypasses the SPI driver entirely.
 * Requires s_dac_hw to be set and the SPI peripheral to be configured by a
 * prior driver call.  Measured ~0.6 µs per frame at 50 MHz / 240 MHz CPU.
 *
 * Byte layout in data_buf[0] (little-endian register, MSB byte sent first):
 *   bits[ 7: 0] = reg      → first byte on wire
 *   bits[15: 8] = value_hi → second byte
 *   bits[23:16] = value_lo → third byte
 */
static IRAM_ATTR void dac_write_fast(uint8_t reg, uint16_t value)
{
    s_dac_hw->data_buf[0] = (uint32_t)reg
                           | ((uint32_t)(value >> 8)   <<  8)
                           | ((uint32_t)(value & 0xFF) << 16);
    s_dac_hw->cmd.usr = 1;
    while (s_dac_hw->cmd.usr);
}

static IRAM_ATTR void dac_write_sample(const int16_t s[RP_SAMPLE_CHANNELS], bool mx, bool my)
{
    int16_t x = s[0];
    int16_t y = s[1];
    if (mx) { x = (x == -32768) ? 32767 : -x; }
    if (my) { y = (y == -32768) ? 32767 : -y; }
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_X, signed_to_dac(x));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_Y, signed_to_dac(y));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_R, signed_to_dac(s[2]));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_G, signed_to_dac(s[3]));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_B, signed_to_dac(s[4]));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_I,  signed_to_dac(s[5]));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_U1, signed_to_dac(s[6]));
    dac_write_fast(DAC_REG_DAC_BASE + DAC_CH_U2, signed_to_dac(s[7]));
    dac_write_fast(DAC_REG_TRIGGER, 0x0010);
}

static void dac_all_zero(void)
{
    dac_write(DAC_REG_DAC_BASE + DAC_CH_X,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_Y,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_R,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_G,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_B,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_I,  0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_U1, 0);
    dac_write(DAC_REG_DAC_BASE + DAC_CH_U2, 0);
    dac_write(DAC_REG_TRIGGER, 0x0010);
}

static esp_err_t dac_spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = DAC_MOSI_GPIO,
        .miso_io_num     = DAC_SDO_GPIO,
        .sclk_io_num     = DAC_SCLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4,
    };

    esp_err_t ret = spi_bus_initialize(DAC_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = V1_DAC_SPI_CLOCK_HZ,
        .mode           = 1,      /* CPOL=0, CPHA=1 */
        .spics_io_num   = DAC_CS_GPIO,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };

    ret = spi_bus_add_device(DAC_SPI_HOST, &dev, &s_dac_spi);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

static void shutter_set(bool open)
{
    gpio_set_level(DAC_SHUTTER_GPIO, open ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Test shape generation (shared with V2 rp_manager.c)               */
/* ------------------------------------------------------------------ */

static int16_t clamp_i16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

static uint32_t isqrt_u32(uint32_t n)
{
    uint32_t root = 0, bit = 1UL << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else { root >>= 1; }
        bit >>= 2;
    }
    return root;
}

static int32_t lerp_i32(int32_t a, int32_t b, uint32_t pos, uint32_t span)
{
    if (span == 0) return b;
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)pos) / (int64_t)span);
}

static void line_point(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                       uint32_t pos, uint32_t span, int32_t *x, int32_t *y)
{
    *x = lerp_i32(ax, bx, pos, span);
    *y = lerp_i32(ay, by, pos, span);
}

static void build_test_samples(
    int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    const rp_test_shape_t shape = s_test_shape;
    const uint8_t cmask = s_test_color_mask & 0x07;

    for (int i = 0; i < RP_STREAM_SAMPLES_PER_PACKET; i++) {
        uint32_t p = s_test_phase + (uint32_t)i;
        int32_t x = 0, y = 0;
        bool blank = false;

        if (shape == RP_TEST_SHAPE_BEAM) {
            x = y = 0;
        } else if (shape == RP_TEST_SHAPE_CIRCLE) {
            const int32_t r = 32767;
            uint32_t t = p % 1024, q = t & 255u;
            int32_t v = r - (int32_t)((q * (uint32_t)r) / 255u);
            int32_t h = (int32_t)isqrt_u32((uint32_t)(r*r) - (uint32_t)(v*v));
            if      (t < 256u) { x =  v; y =  h; }
            else if (t < 512u) { x = -h; y =  v; }
            else if (t < 768u) { x = -v; y = -h; }
            else               { x =  h; y = -v; }
        } else if (shape == RP_TEST_SHAPE_CROSS) {
            uint32_t t = p % 1024;
            if      (t < 448u) { line_point(-32768,0,32767,0,t,447u,&x,&y); }
            else if (t < 512u) { blank=true; line_point(32767,0,0,-32768,t-448u,63u,&x,&y); }
            else if (t < 960u) { line_point(0,-32768,0,32767,t-512u,447u,&x,&y); }
            else               { blank=true; line_point(0,32767,-32768,0,t-960u,63u,&x,&y); }
        } else if (shape == RP_TEST_SHAPE_HEART) {
            uint32_t t = p % 1536, seg = t / 256u, q = t & 255u;
            switch (seg) {
            case 0: line_point(0,12000,-21000,30000,q,255u,&x,&y); break;
            case 1: line_point(-21000,30000,-32768,5000,q,255u,&x,&y); break;
            case 2: line_point(-32768,5000,0,-32768,q,255u,&x,&y); break;
            case 3: line_point(0,-32768,32767,5000,q,255u,&x,&y); break;
            case 4: line_point(32767,5000,21000,30000,q,255u,&x,&y); break;
            default:line_point(21000,30000,0,12000,q,255u,&x,&y); break;
            }
        } else { /* SQUARE */
            uint32_t t = p % 1280;
            if      (t < 256u)  { line_point(-32768,-32768,32767,-32768,t,255u,&x,&y); }
            else if (t < 512u)  { line_point(32767,-32768,32767,32767,t-256u,255u,&x,&y); }
            else if (t < 768u)  { line_point(32767,32767,-32768,32767,t-512u,255u,&x,&y); }
            else if (t < 1024u) { line_point(-32768,32767,-32768,-32768,t-768u,255u,&x,&y); }
            else { blank=true; x=-32768; y=-32768; }
        }

        samples[i][0] = clamp_i16(x);
        samples[i][1] = clamp_i16(y);
        samples[i][2] = (!blank && (cmask & 0x01)) ? 32767 : -32768;
        samples[i][3] = (!blank && (cmask & 0x02)) ? 32767 : -32768;
        samples[i][4] = (!blank && (cmask & 0x04)) ? 32767 : -32768;
        samples[i][5] = (!blank && cmask)           ? 32767 : -32768;
        samples[i][6] = 0;
        samples[i][7] = 0;
    }

    s_test_phase += RP_STREAM_SAMPLES_PER_PACKET;
}

/* ------------------------------------------------------------------ */
/* Output task - runs on core 1, clocks samples out at configured rate */
/* ------------------------------------------------------------------ */

static void output_task(void *arg)
{
    (void)arg;

    int16_t sample[RP_SAMPLE_CHANNELS];
    int16_t hold_x = 0, hold_y = 0;
    uint32_t active_rate = s_status.remote_sample_rate;
    if (active_rate == 0) {
        active_rate = 1;
    }
    uint32_t idle_yield_count = 0;
    uint64_t period_q32 = ((uint64_t)1000000 << 32) / active_rate;
    uint64_t next_q32 = ((uint64_t)esp_timer_get_time() << 32);

    while (s_output_running) {
        uint32_t requested_rate = s_status.remote_sample_rate;
        /* Small changes are IDN jitter, but never stay above the limit. */
        if (requested_rate > 0 &&
            (requested_rate > active_rate + 500 || active_rate > requested_rate + 500 ||
             active_rate > s_status.rate_limit)) {
            active_rate = requested_rate;
            period_q32 = ((uint64_t)1000000 << 32) / active_rate;
            /* Don't reset next_q32 — let the resync check below absorb any
             * drift caused by the period change.  Hard-resetting next_q32
             * every time IDN jitters the rate causes a visible timing glitch. */
        }

        int64_t due_us = (int64_t)(next_q32 >> 32);
        while (s_output_running) {
            int64_t wait_us = due_us - esp_timer_get_time();
            if (wait_us <= 0) {
                break;
            }
            if (wait_us > 2000) {
                vTaskDelay(delay_ticks_ms(1));
            }
        }

        bool mx = s_status.remote_mirror_x;
        bool my = s_status.remote_mirror_y;

        if (ring_pop(sample)) {
            hold_x = sample[0];
            hold_y = sample[1];
            dac_write_sample(sample, mx, my);
        } else {
            s_status.remote_underruns++;
            /* Galvo hold with blanked laser */
            int16_t blank[RP_SAMPLE_CHANNELS] = {
                hold_x, hold_y, -32768, -32768, -32768, -32768, -32768, -32768
            };
            dac_write_sample(blank, mx, my);
        }

        s_status.remote_buffer_fill_samples = ring_fill();

        next_q32 += period_q32;
        uint64_t now_q32 = (uint64_t)esp_timer_get_time() << 32;
        if (next_q32 + (period_q32 * V1_OUTPUT_RESYNC_PERIODS) < now_q32) {
            next_q32 = now_q32 + period_q32;
        }

        /* Every 4096 samples, blank the laser and sleep 1 tick so IDLE1 can
         * process any task-cleanup work.  The blank is written BEFORE the
         * sleep so the DAC does not hold a lit sample position during the
         * 1 ms gap.  After the sleep the resync check above will realign
         * next_q32 to the current clock and output continues immediately. */
        if ((++idle_yield_count & 0x0FFFu) == 0) {
            int16_t b[RP_SAMPLE_CHANNELS];
            b[0] = hold_x; b[1] = hold_y;
            for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++) b[ch] = -32768;
            dac_write_sample(b, mx, my);
            vTaskDelay(1);
        }
    }

    shutter_set(false);   /* close before zeroing so galvos don't snap to 0V while lit */
    dac_all_zero();
    resume_core1_idle_wdt();

    s_status.remote_running = false;
    s_output_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Test stream task                                                    */
/* ------------------------------------------------------------------ */

static void test_stream_task(void *arg)
{
    (void)arg;

    static int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];

    while (s_test_stream_requested) {
        if (ring_space() < (RP_STREAM_SAMPLES_PER_PACKET * 2u)) {
            vTaskDelay(delay_ticks_ms(1));
            continue;
        }

        build_test_samples(samples);

        esp_err_t ret = ring_push_packet(samples, 200);
        if (ret != ESP_OK) {
            vTaskDelay(delay_ticks_ms(5));
        } else {
            s_status.test_stream_blocks_sent++;
        }

        vTaskDelay(delay_ticks_ms(1));
    }

    s_status.test_stream_running = false;
    s_test_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API (same signatures as rp_manager.h)                       */
/* ------------------------------------------------------------------ */

esp_err_t rp_manager_start(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    if (!s_push_mutex) {
        s_push_mutex = xSemaphoreCreateMutex();
        if (!s_push_mutex) return ESP_ERR_NO_MEM;
    }

    /* Shutter GPIO - default closed */
    gpio_config_t sh = {
        .pin_bit_mask  = 1ULL << DAC_SHUTTER_GPIO,
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sh));
    shutter_set(false);

    esp_err_t ret = dac_spi_init();
    if (ret != ESP_OK) {
        s_status.last_error = ret;
        return ret;
    }

    /* DAC init sequence: soft reset, GAIN=0x00FF, SYNC=0xFFFF */
    vTaskDelay(pdMS_TO_TICKS(10));
    dac_write(DAC_REG_TRIGGER, 0x000A);   /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(10));
    dac_write(DAC_REG_GAIN,    0x00FF);   /* all channels gain = 1x */
    dac_write(DAC_REG_SYNC,    0xFFFF);   /* all channels synchronous */
    dac_all_zero();

    /* Capture the hardware register pointer now that the driver has fully
     * configured the SPI peripheral.  dac_write_fast uses this directly. */
    s_dac_hw = SPI_LL_GET_HW(DAC_SPI_HOST);

    s_status.initialized = true;
    s_output_ref = 0;

    rate_limit_load();
    if (s_status.remote_sample_rate > s_status.rate_limit) {
        s_status.remote_sample_rate = s_status.rate_limit;
    }

    ESP_LOGI(TAG, "DAC80508 ready CS=%d SCK=%d MOSI=%d shutter=%d",
             DAC_CS_GPIO, DAC_SCLK_GPIO, DAC_MOSI_GPIO, DAC_SHUTTER_GPIO);
    ESP_LOGI(TAG, "Scanner rate limit %lu pps", (unsigned long)s_status.rate_limit);

    return ESP_OK;
}

esp_err_t rp_manager_ping(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_status.ping_count++;
    return ESP_OK;
}

esp_err_t rp_manager_get_remote_status(void)
{
    s_status.remote_buffer_fill_samples = ring_fill();
    return ESP_OK;
}

esp_err_t rp_manager_output_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_output_ref == 0) {
        if (s_output_task_handle) {
            s_output_running = false;
            xSemaphoreGive(s_mutex);

            esp_err_t ret = wait_output_task_stopped(V1_STOP_WAIT_MS);
            if (ret != ESP_OK) {
                return ret;
            }

            xSemaphoreTake(s_mutex, portMAX_DELAY);
        }

        s_output_running = true;
        s_status.remote_running = true;
        s_status.remote_underruns = 0;

        esp_err_t wdt_ret = suspend_core1_idle_wdt();
        if (wdt_ret != ESP_OK) {
            s_output_running = false;
            s_status.remote_running = false;
            xSemaphoreGive(s_mutex);
            return wdt_ret;
        }

        shutter_set(true);

        BaseType_t ok = xTaskCreatePinnedToCore(
            output_task, "dac_out", V1_OUTPUT_TASK_STACK, NULL, 6, &s_output_task_handle, 1);

        if (ok != pdPASS) {
            s_output_running = false;
            s_status.remote_running = false;
            s_output_ref = 0;
            shutter_set(false);
            resume_core1_idle_wdt();
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }

    }

    s_output_ref++;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t rp_manager_output_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_output_ref > 0 && --s_output_ref == 0) {
        s_output_running = false;
        ring_clear();   /* prevent ERR_TIMEOUT on next output_start */
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t rp_manager_output_force_stop(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_output_ref = 0;
    s_output_running = false;
    xSemaphoreGive(s_mutex);

    esp_err_t ret = wait_output_task_stopped(V1_STOP_WAIT_MS);
    ring_clear();
    return ret;
}

esp_err_t rp_manager_clear_buffer(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;
    s_output_running = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_output_ref = 0;
    xSemaphoreGive(s_mutex);

    esp_err_t ret = wait_output_task_stopped(V1_STOP_WAIT_MS);
    ring_clear();
    return ret;
}

/*
   Drop samples a producer queued but never STARTed, so the next START does
   not play them. Does nothing while another producer holds the output,
   since its samples share the same ring. The push mutex keeps a producer
   from writing while the ring is reset.
*/
esp_err_t rp_manager_output_discard_unstarted(void)
{
    if (!s_mutex || !s_push_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_output_ref == 0) {
        xSemaphoreTake(s_push_mutex, portMAX_DELAY);
        ring_clear();
        xSemaphoreGive(s_push_mutex);
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t rp_manager_set_rate_applied(uint32_t rate_hz, uint32_t *applied)
{
    if (rate_hz < 1) rate_hz = 1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_status.remote_requested_rate = rate_hz;

    if (rate_hz > s_status.rate_limit) {
        ESP_LOGW(TAG, "Rate %lu above scanner limit %lu; clamped",
                 (unsigned long)rate_hz, (unsigned long)s_status.rate_limit);
        rate_hz = s_status.rate_limit;
    }

    if (rate_hz > V1_MAX_RATE_HZ) rate_hz = V1_MAX_RATE_HZ;

    s_status.remote_sample_rate = rate_hz;
    xSemaphoreGive(s_mutex);

    if (applied) {
        *applied = rate_hz;
    }

    return ESP_OK;
}

esp_err_t rp_manager_set_rate(uint32_t rate_hz)
{
    return rp_manager_set_rate_applied(rate_hz, NULL);
}

uint32_t rp_manager_get_rate_limit(void)
{
    return s_status.rate_limit;
}

uint32_t rp_manager_get_sample_rate(void)
{
    return s_status.remote_sample_rate;
}

esp_err_t rp_manager_set_rate_limit(uint32_t limit_hz)
{
    limit_hz = clamp_rate_limit(limit_hz);

    esp_err_t ret = rate_limit_save(limit_hz);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Rate limit not saved: %s", esp_err_to_name(ret));
        return ret;
    }

    s_status.rate_limit = limit_hz;
    s_status.remote_max_rate = limit_hz;
    ESP_LOGI(TAG, "Scanner rate limit set to %lu pps", (unsigned long)limit_hz);

    /* Re-apply the producer's rate under the new limit, raised or lowered. */
    if (s_status.remote_requested_rate > 0) {
        rp_manager_set_rate(s_status.remote_requested_rate);
    }

    return ESP_OK;
}

/* V1 has no colour delay line; the web page shows the error. */
esp_err_t rp_manager_set_color_shift(int32_t shift_us)
{
    (void)shift_us;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rp_manager_save_color_shift(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rp_manager_set_buffer_config(uint32_t size_samples,
                                       uint32_t high_water_samples,
                                       uint32_t low_water_samples)
{
    /* V1 has a fixed ring buffer; store watermarks for UI display only */
    if (high_water_samples >= V1_RING_SIZE) high_water_samples = V1_RING_SIZE - 1;
    if (low_water_samples  >= high_water_samples) low_water_samples = 0;

    s_status.remote_buffer_size_samples       = V1_RING_SIZE;
    s_status.remote_buffer_max_samples        = V1_RING_SIZE;
    s_status.remote_buffer_high_water_samples = high_water_samples;
    s_status.remote_buffer_low_water_samples  = low_water_samples;

    (void)size_samples;
    return ESP_OK;
}

esp_err_t rp_manager_set_mirror(bool mirror_x, bool mirror_y)
{
    s_status.remote_mirror_x = mirror_x;
    s_status.remote_mirror_y = mirror_y;
    return ESP_OK;
}

void rp_manager_set_realtime_streaming(bool enabled)
{
    (void)enabled;
}

esp_err_t rp_manager_send_test_block(void)
{
    static int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];
    build_test_samples(samples);
    return ring_push_packet(samples, 200);
}

esp_err_t rp_manager_start_test_stream(void)
{
    return rp_manager_start_test_shape(RP_TEST_SHAPE_SQUARE);
}

esp_err_t rp_manager_start_test_shape(rp_test_shape_t shape)
{
    /* No colour delay line on V1, so nothing for this pattern to calibrate. */
    if (shape == RP_TEST_SHAPE_COLOR_SHIFT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_test_shape = shape;
    s_test_phase = 0;
    s_test_stream_requested = true;
    s_status.test_stream_running = true;

    if (s_test_task_handle) {
        return rp_manager_output_start();
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        test_stream_task, "dac_test", V1_TEST_TASK_STACK, NULL, 3, &s_test_task_handle, 0);

    if (ok != pdPASS) {
        s_test_stream_requested = false;
        s_status.test_stream_running = false;
        s_test_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return rp_manager_output_start();
}

void rp_manager_set_test_color_mask(uint8_t mask)
{
    s_test_color_mask = mask & 0x07;
}

esp_err_t rp_manager_stop_test_stream(void)
{
    s_test_stream_requested = false;
    s_status.test_stream_running = false;

    uint32_t waited = 0;
    while (s_test_task_handle && waited < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    return s_test_task_handle ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t rp_manager_send_samples_packet(
    const int16_t samples[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS])
{
    if (!s_push_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_push_mutex, portMAX_DELAY);
    esp_err_t ret = ring_push_packet(samples, 500);
    xSemaphoreGive(s_push_mutex);

    if (ret != ESP_OK) {
        s_status.error_count++;
        s_status.last_error = ret;
    }

    return ret;
}

esp_err_t rp_manager_wait_payload_idle(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

void rp_manager_get_status(rp_status_t *status)
{
    if (!status) return;

    s_status.remote_buffer_fill_samples = ring_fill();
    *status = s_status;
}
