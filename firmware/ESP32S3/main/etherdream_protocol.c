/*
 * etherdream_protocol.c
 *
 * Full Ether Dream DAC protocol implementation for ESP32-S3 + RP2350 Laser DAC.
 *
 * Architecture:
 *   - UDP broadcast task:  1 Hz discovery broadcast on port 7654
 *   - TCP listener task:   accepts one client at a time on port 7765
 *   - TCP client task:     handles the full Ether Dream state machine
 *   - Feed task:           drains ring-buffer -> RP2350, core 1, priority 6
 *
 * Ring buffer:
 *   SPSC lock-free ring of ed_point_t (18 bytes each).
 *   Indices live in [0, 2*CAPACITY) to distinguish empty from full.
 *   head written only by consumer (feed task); tail only by producer (client task).
 *
 * Point conversion  (Ether Dream -> RP sample):
 *   ch[0] = X            (int16, direct copy)
 *   ch[1] = Y            (int16, direct copy)
 *   ch[2] = R            (uint16 -> int16: val - 32768)
 *   ch[3] = G
 *   ch[4] = B
 *   ch[5] = I            (intensity)
 *   ch[6] = 0
 *   ch[7] = 0
 *
 * Underrun protection:
 *   Feed task pads any short packet with the last known point (galvo hold).
 *   rp_manager_output_start() is called only after the first successful send.
 *
 * Reconnect safety:
 *   On client disconnect (normal, timeout, or E-Stop), ALL state is reset to
 *   Idle/Ready so the next client can Prepare without a board reboot.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"

#include "app_config.h"
#include "rp_manager.h"
#include "etherdream_protocol.h"
#if BOARD_VERSION == 1
#include "ilda_player.h"
#endif
#include "protocol_stream_stats.h"

/* -- Tunables --------------------------------------------------------------- */

/* Ring size in points. Must be a power of 2. One slot stays empty, so
 * clients are told ED_BUFFER_CAPACITY. */
#define ED_RING_CAPACITY        8192u
#define ED_BUFFER_CAPACITY      (ED_RING_CAPACITY - 1u)
#define ED_FEED_FLUSH_US        100000LL    /* send a partial packet only after the stream idles this long */
#define ED_FEED_PREBUFFER       3072u       /* samples queued on the RP before START (~100 ms at 30 kpps) */
#define ED_RP_CUSHION           ED_FEED_PREBUFFER /* samples kept queued on the RP while playing */
#define ED_RP_POLL_MS           100u        /* re-read the RP's buffer fill this often */

/* Maximum point rate we advertise and accept. */
#define ED_MAX_POINT_RATE       100000u

/* Set to 1 to log per-second client pacing while playing. */
#ifndef ED_DIAG
#define ED_DIAG                 0
#endif

/* Default rate used when Begin sends 0. */
#define ED_DEFAULT_POINT_RATE   30000u

/* Ports */
#define ED_TCP_PORT             ETHERDREAM_PROTOCOL_TCP_PORT
#define ED_UDP_BCAST_PORT       ETHERDREAM_PROTOCOL_UDP_PORT

/* Task stack sizes (bytes) */
#define STACK_BROADCAST         3072u
#define STACK_LISTENER          3072u
#define STACK_CLIENT            12288u   /* pt_chunk[128] lives on stack */
#define STACK_FEED              4096u
#define STACK_RP_POLL           3072u

#if BOARD_VERSION == 1
#define ED_FEED_TASK_CORE     0
#define ED_FEED_TASK_PRIORITY 5
#define ED_LISTENER_TASK_PRIORITY 6
#define ED_CLIENT_TASK_PRIORITY   6
#else
#define ED_FEED_TASK_CORE     1
#define ED_FEED_TASK_PRIORITY 6
#define ED_LISTENER_TASK_PRIORITY 4
#define ED_CLIENT_TASK_PRIORITY   5
#endif

/* -- Ether Dream protocol structures (little-endian, packed) --------------- */
#pragma pack(push, 1)

typedef struct {
    uint8_t  protocol;
    uint8_t  light_engine_state;
    uint8_t  playback_state;
    uint8_t  source;
    uint16_t light_engine_flags;
    uint16_t playback_flags;
    uint16_t source_flags;
    uint16_t buffer_fullness;
    uint32_t point_rate;
    uint32_t point_count;
} ed_dac_status_t;

typedef struct {
    uint8_t         mac_address[6];
    uint16_t        hw_revision;
    uint16_t        sw_revision;
    uint16_t        buffer_capacity;
    uint32_t        max_point_rate;
    ed_dac_status_t status;
} ed_broadcast_t;

typedef struct {
    uint8_t         response;   /* 'a' ACK  'F' Full  'I' Invalid  '!' EStop */
    uint8_t         command;
    ed_dac_status_t status;
} ed_response_t;

typedef struct {
    uint16_t control;
    int16_t  x;
    int16_t  y;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t i;
    uint16_t u1;
    uint16_t u2;
} ed_point_t;

typedef struct {
    uint8_t  command;
    uint16_t low_water_mark;
    uint32_t point_rate;
} ed_begin_cmd_t;

typedef struct {
    uint8_t  command;
    uint32_t point_rate;
} ed_queue_rate_cmd_t;

typedef struct {
    uint8_t  command;
    uint16_t npoints;
} ed_data_cmd_hdr_t;

#pragma pack(pop)

/* -- State constants -------------------------------------------------------- */
#define LE_READY    0
#define LE_ESTOP    3

#define PB_IDLE     0
#define PB_PREPARED 1
#define PB_PLAYING  2

#define RESP_ACK  'a'
#define RESP_FULL 'F'
#define RESP_INV  'I'

#define RATE_QUEUE_CAP 4

/* -- Module state ----------------------------------------------------------- */
static const char *TAG = "etherdream";

static uint8_t s_mac[6];

/* Playback state (written by client task, read by feed task + broadcast task) */
static volatile uint8_t  s_le_state    = LE_READY;
static volatile uint8_t  s_pb_state    = PB_IDLE;
static volatile uint16_t s_pb_flags    = 0;
static volatile uint32_t s_point_count = 0;
static volatile uint32_t s_rp_queued   = 0;   /* estimated samples queued on the RP */

/*
   RP fill snapshot, refreshed over UART by rp_poll_task so the feed task
   never blocks on a status round trip. s_rp_sent counts packet samples
   sent since RP output started.
*/
static portMUX_TYPE      s_rp_poll_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t           s_rp_poll_us;
static uint32_t          s_rp_poll_fill;
static uint32_t          s_rp_poll_sent;
static uint32_t          s_rp_poll_underruns;         /* last RP underrun count seen */
static bool              s_rp_poll_underruns_valid;
static volatile uint32_t s_rp_sent;
static volatile bool     s_rp_output_started = false;
static TaskHandle_t      s_rp_poll_task_h = NULL;

/* Feed control */
static volatile bool  s_output_armed = false;
static volatile bool  s_feed_running = false;
static TaskHandle_t   s_feed_task_h  = NULL;

/* Client control */
static TaskHandle_t   s_client_task_h = NULL;
static volatile int   s_client_sock   = -1;
#if BOARD_VERSION == 1
static QueueHandle_t  s_client_sock_q = NULL;
static volatile bool  s_client_active = false;
#endif

/* -- SPSC ring buffer ------------------------------------------------------- */
static ed_point_t        *s_ring      = NULL;
static volatile uint32_t  s_ring_head = 0;   /* consumer (feed task) */
static volatile uint32_t  s_ring_tail = 0;   /* producer (client task) */

static inline uint32_t ring_size(void)
{
    uint32_t h = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE);
    return (t - h + ED_RING_CAPACITY * 2u) % (ED_RING_CAPACITY * 2u);
}

static inline uint32_t ring_free(void)
{
    return ED_RING_CAPACITY - 1u - ring_size();
}

static uint32_t ring_write(const ed_point_t *pts, uint32_t n)
{
    uint32_t free_space = ring_free();
    if (n > free_space) n = free_space;
    uint32_t tail = s_ring_tail;
    uint32_t mask = ED_RING_CAPACITY - 1u;
    uint32_t wrap = ED_RING_CAPACITY * 2u;
    for (uint32_t i = 0; i < n; i++)
        s_ring[(tail + i) & mask] = pts[i];
    __atomic_store_n(&s_ring_tail, (tail + n) % wrap, __ATOMIC_RELEASE);
    return n;
}

static uint32_t ring_read(ed_point_t *out, uint32_t n)
{
    uint32_t avail = ring_size();
    if (n > avail) n = avail;
    uint32_t head = s_ring_head;
    uint32_t mask = ED_RING_CAPACITY - 1u;
    uint32_t wrap = ED_RING_CAPACITY * 2u;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_ring[(head + i) & mask];
    __atomic_store_n(&s_ring_head, (head + n) % wrap, __ATOMIC_RELEASE);
    return n;
}

static void ring_clear(void)
{
    s_ring_head = 0;
    s_ring_tail = 0;
}

/* -- Point conversion ------------------------------------------------------- */
static inline void ed_point_to_rp(const ed_point_t *pt,
                                   int16_t out[RP_SAMPLE_CHANNELS])
{
    out[0] = pt->x;
    out[1] = pt->y;
    /* Ether Dream: 0-65535 unsigned -> RP int16: subtract 32768 */
    out[2] = (int16_t)((int32_t)(uint32_t)pt->r - 32768);
    out[3] = (int16_t)((int32_t)(uint32_t)pt->g - 32768);
    out[4] = (int16_t)((int32_t)(uint32_t)pt->b - 32768);
    out[5] = (int16_t)((int32_t)(uint32_t)pt->i - 32768);
    out[6] = 0;
    out[7] = 0;
}

/* -- Status snapshot -------------------------------------------------------- */
static ed_dac_status_t make_status(void)
{
    ed_dac_status_t s = {0};
    s.light_engine_state = s_le_state;
    s.playback_state     = s_pb_state;
    s.playback_flags     = s_pb_flags;
    /*
       Report everything not yet played, including the RP's cushion, so the
       client paces against the real queue. Capped at capacity so the free
       space a client computes never exceeds what the ring can take.
    */
    uint32_t fill        = ring_size() + s_rp_queued;
    if (fill > ED_BUFFER_CAPACITY) fill = ED_BUFFER_CAPACITY;
    s.buffer_fullness    = (uint16_t)fill;
    s.point_rate         = (s_pb_state == PB_IDLE) ? 0 : rp_manager_get_sample_rate();
    s.point_count        = s_point_count;
    return s;
}

/* -- TCP helpers ------------------------------------------------------------ */
static bool tcp_recv_all(int sock, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        int r = recv(sock, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

static bool tcp_send_all(int sock, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        int r = send(sock, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

static bool send_response(int sock, uint8_t resp_code, uint8_t cmd)
{
    ed_response_t r;
    r.response = resp_code;
    r.command  = cmd;
    r.status   = make_status();
    return tcp_send_all(sock, &r, sizeof(r));
}

/* -- RP fill estimate ------------------------------------------------------- */
static void rp_snapshot_set(int64_t at_us, uint32_t fill, uint32_t sent)
{
    portENTER_CRITICAL(&s_rp_poll_mux);
    s_rp_poll_us   = at_us;
    s_rp_poll_fill = fill;
    s_rp_poll_sent = sent;
    portEXIT_CRITICAL(&s_rp_poll_mux);
}

/* Samples still queued on the RP: last reading + sent since - played since. */
static uint32_t rp_queued_estimate(void)
{
    portENTER_CRITICAL(&s_rp_poll_mux);
    int64_t  at_us = s_rp_poll_us;
    uint32_t fill  = s_rp_poll_fill;
    uint32_t sent  = s_rp_poll_sent;
    portEXIT_CRITICAL(&s_rp_poll_mux);

    /* The RP's rate, not the client's: a scanner limit change moves it mid-stream. */
    int64_t played = (int64_t)rp_manager_get_sample_rate() * (esp_timer_get_time() - at_us) / 1000000;
    int64_t queued = (int64_t)fill + (uint32_t)(s_rp_sent - sent) - played;
    return queued > 0 ? (uint32_t)queued : 0u;
}

static void rp_poll_task(void *arg)
{
    (void)arg;

    while (s_feed_running) {
        if (s_rp_output_started) {
            /*
               Time taken before the round trip and the sent count after it:
               both errors then under-count the RP's queue, so the feed
               sends early rather than starving it.
            */
            int64_t at_us = esp_timer_get_time();
            if (rp_manager_get_remote_status() == ESP_OK) {
                rp_status_t rp = {0};
                rp_manager_get_status(&rp);
                rp_snapshot_set(at_us, rp.remote_buffer_fill_samples, s_rp_sent);

                /*
                   The RP blanked samples because its ring ran dry: report
                   it in playback_flags as the real DAC does (bit 1), so a
                   client can see that it fell behind. Cleared by Prepare.
                */
                if (s_rp_poll_underruns_valid &&
                    rp.remote_underruns != s_rp_poll_underruns &&
                    s_pb_state == PB_PLAYING) {
                    s_pb_flags |= (1u << 1);
                }
                s_rp_poll_underruns = rp.remote_underruns;
                s_rp_poll_underruns_valid = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ED_RP_POLL_MS));
    }

    s_rp_poll_task_h = NULL;
    vTaskDelete(NULL);
}

/* -- Feed task -------------------------------------------------------------- */
/* DMA-aligned packet buffer and staging area in static storage */
static int16_t    s_feed_packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];
static ed_point_t s_feed_stage[RP_STREAM_SAMPLES_PER_PACKET];

/* START the RP on the samples already queued and seed the queue estimate. */
static void feed_output_start(uint32_t queued)
{
    rp_manager_output_start();
    s_rp_sent = 0;
    rp_snapshot_set(esp_timer_get_time(), queued, 0);
    s_rp_poll_underruns_valid = false;
    s_rp_output_started = true;
    ESP_LOGI(TAG, "RP output started");
}

static void feed_task(void *arg)
{
    (void)arg;
    s_feed_running = true;

    /* Last-known point for padding a flushed tail */
    ed_point_t last = {0};

    bool output_started = false;
    uint32_t queued_before_start = 0;
    uint32_t last_avail = 0;
    int64_t last_growth_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Feed task started");

    while (s_feed_running) {

        if (s_pb_state != PB_PLAYING || !s_output_armed) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /*
           Keep only a short cushion on the RP and leave the rest of the
           queue in the ring, where the client's buffer_fullness can see it.
        */
        if (output_started) {
            s_rp_queued = rp_queued_estimate();

            if (s_rp_queued + RP_STREAM_SAMPLES_PER_PACKET > ED_RP_CUSHION) {
                vTaskDelay(1);
                continue;
            }
        }

        /*
           Send only full packets while the stream is flowing. Padding a
           partial packet splices filler into the drawing, and the RP buffer
           is deep enough to wait for more points. A partial packet is sent
           only once the stream has gone idle, to flush its tail; an empty
           ring sends nothing and the RP blanks on underrun.
        */
        uint32_t avail   = ring_size();
        uint32_t to_read = RP_STREAM_SAMPLES_PER_PACKET;

        if (avail < RP_STREAM_SAMPLES_PER_PACKET) {
            int64_t now_us = esp_timer_get_time();

            if (avail != last_avail) {
                last_avail = avail;
                last_growth_us = now_us;
            }

            bool idle = now_us - last_growth_us >= ED_FEED_FLUSH_US;

            if (avail == 0 || !idle) {
                /* A stream that idles on a packet boundary has no tail to flush; start what is queued. */
                if (avail == 0 && idle && !output_started && queued_before_start > 0) {
                    feed_output_start(queued_before_start);
                    output_started = true;
                }
                vTaskDelay(1);
                continue;
            }

            to_read = avail;
        }

        uint32_t got = ring_read(s_feed_stage, to_read);
        last_avail = ring_size();
        last_growth_us = esp_timer_get_time();

        /* Convert real samples */
        for (uint32_t i = 0; i < got; i++) {
            ed_point_to_rp(&s_feed_stage[i], s_feed_packet[i]);
            last = s_feed_stage[i];
        }

        /* Pad a flushed tail by holding position with the laser blanked. */
        int16_t pad[RP_SAMPLE_CHANNELS];
        ed_point_to_rp(&last, pad);
        for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++)
            pad[ch] = -32768;
        for (uint32_t i = got; i < RP_STREAM_SAMPLES_PER_PACKET; i++)
            memcpy(s_feed_packet[i], pad, sizeof(pad));

        esp_err_t err = rp_manager_send_samples_packet(s_feed_packet);
        if (err == ESP_OK) {
            queued_before_start += got;

            /* Start once a short cushion is queued, or when a short stream is flushed. */
            if (!output_started &&
                (queued_before_start >= ED_FEED_PREBUFFER || got < RP_STREAM_SAMPLES_PER_PACKET)) {
                feed_output_start(queued_before_start);
                output_started = true;
            } else if (output_started) {
                s_rp_sent += RP_STREAM_SAMPLES_PER_PACKET;
            }
            s_point_count += got;
            protocol_stream_stats_add_samples(PROTOCOL_STREAM_ETHERDREAM, got);
        } else {
            ESP_LOGW(TAG, "send_samples_packet: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        taskYIELD();
    }

    /* Release only a START this feed made; otherwise drop its prebuffer. */
    if (output_started)
        rp_manager_output_stop();
    else if (queued_before_start > 0)
        rp_manager_output_discard_unstarted();
    s_rp_output_started = false;
    s_rp_queued = 0;
    ESP_LOGI(TAG, "Feed task exiting");
    s_feed_task_h = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_feed_task(void)
{
    if (s_feed_task_h != NULL) return ESP_OK;
    s_feed_running = true;
    s_rp_output_started = false;
    BaseType_t ok = xTaskCreatePinnedToCore(
        feed_task, "ed_feed", STACK_FEED, NULL, ED_FEED_TASK_PRIORITY, &s_feed_task_h, ED_FEED_TASK_CORE);
    if (ok != pdPASS) {
        s_feed_running = false;
        s_feed_task_h  = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_rp_poll_task_h == NULL) {
        ok = xTaskCreatePinnedToCore(
            rp_poll_task, "ed_rp_poll", STACK_RP_POLL, NULL, ED_FEED_TASK_PRIORITY - 1,
            &s_rp_poll_task_h, ED_FEED_TASK_CORE);
        if (ok != pdPASS) {
            s_rp_poll_task_h = NULL;
            ESP_LOGW(TAG, "RP poll task not started; pacing by clock only");
        }
    }
    return ESP_OK;
}

static void stop_feed_task(void)
{
    s_feed_running = false;
    s_output_armed = false;
    for (int i = 0; i < 100 && (s_feed_task_h || s_rp_poll_task_h); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

/* -- Pacing diagnostics ----------------------------------------------------- */
#if ED_DIAG
static struct {
    int64_t  window_start_us;
    uint32_t data_cmds, points, pings, full_naks;
    uint32_t fullness_min, fullness_max;
    uint64_t fullness_sum;
    int64_t  wait_us, wait_max_us, payload_us;
} s_diag;

static void diag_reset(int64_t now_us)
{
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.window_start_us = now_us;
    s_diag.fullness_min = UINT32_MAX;
}

static void diag_maybe_log(int64_t now_us)
{
    int64_t span = now_us - s_diag.window_start_us;
    if (span < 1000000) return;
    if (s_pb_state == PB_PLAYING && s_diag.data_cmds > 0) {
        rp_status_t rp = {0};
        rp_manager_get_status(&rp);
        ESP_LOGI(TAG, "diag %lu pps: d=%lu pts=%lu (%lu/s) avg=%lu ping=%lu full=%lu "
                 "fullness %lu..%lu avg %lu, wait %lldms max %lldms, payload %lldms, rp_fill=%lu est=%lu",
                 (unsigned long)rp_manager_get_sample_rate(),
                 (unsigned long)s_diag.data_cmds, (unsigned long)s_diag.points,
                 (unsigned long)(s_diag.points * 1000000ull / (uint64_t)span),
                 (unsigned long)(s_diag.points / s_diag.data_cmds),
                 (unsigned long)s_diag.pings, (unsigned long)s_diag.full_naks,
                 (unsigned long)s_diag.fullness_min, (unsigned long)s_diag.fullness_max,
                 (unsigned long)(s_diag.fullness_sum / s_diag.data_cmds),
                 s_diag.wait_us / 1000, s_diag.wait_max_us / 1000, s_diag.payload_us / 1000,
                 (unsigned long)rp.remote_buffer_fill_samples, (unsigned long)s_rp_queued);
    }
    diag_reset(now_us);
}
#endif

/* -- Client handling -------------------------------------------------------- */
static void handle_client(int sock)
{
    s_client_sock = sock;
    ESP_LOGI(TAG, "Client connected (sock=%d)", sock);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send initial status on connect (per spec) */
    if (!send_response(sock, RESP_ACK, '?'))
        goto disconnect;

    {
        /* Per-connection rate queue */
        uint32_t rate_queue[RATE_QUEUE_CAP] = {0};
        int rq_head = 0, rq_tail = 0, rq_count = 0;
#if ED_DIAG
        diag_reset(esp_timer_get_time());
#endif

        while (true) {
            uint8_t cmd = 0;
#if ED_DIAG
            int64_t wait_start_us = esp_timer_get_time();
#endif
            if (!tcp_recv_all(sock, &cmd, 1)) {
                ESP_LOGI(TAG, "Client disconnected or timeout");
                break;
            }
#if ED_DIAG
            int64_t cmd_us = esp_timer_get_time();
            int64_t waited = cmd_us - wait_start_us;
            s_diag.wait_us += waited;
            if (waited > s_diag.wait_max_us) s_diag.wait_max_us = waited;
            if (cmd == '?') s_diag.pings++;
            diag_maybe_log(cmd_us);
#endif

            /* -- Ping -------------------------------------------------- */
            if (cmd == '?') {
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Emergency stop ---------------------------------------- */
            if (cmd == 0x00 || cmd == 0xFF) {
                ESP_LOGW(TAG, "E-Stop received");
                s_le_state     = LE_ESTOP;
                s_pb_state     = PB_IDLE;
                s_pb_flags    |= (1u << 2);
                s_output_armed = false;
                stop_feed_task();
                ring_clear();
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Clear E-Stop ------------------------------------------- */
            if (cmd == 'c') {
                if (s_le_state == LE_ESTOP) {
                    s_le_state  = LE_READY;
                    s_pb_flags &= ~(1u << 2);
                    if (!send_response(sock, RESP_ACK, cmd)) break;
                } else {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                }
                continue;
            }

            /* -- Prepare ----------------------------------------------- */
            if (cmd == 'p') {
                if (s_le_state != LE_READY || s_pb_state != PB_IDLE) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
#if BOARD_VERSION == 1
                rp_manager_output_force_stop();
                rp_manager_clear_buffer();
#endif
                ring_clear();
                s_point_count  = 0;
                s_pb_state     = PB_PREPARED;
                s_pb_flags    &= ~(3u << 1);   /* clear underflow + estop */
                s_output_armed = false;
                if (start_feed_task() != ESP_OK) {
                    s_pb_state = PB_IDLE;
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                ESP_LOGI(TAG, "Prepared");
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Begin playback ---------------------------------------- */
            if (cmd == 'b') {
                ed_begin_cmd_t begin;
                if (!tcp_recv_all(sock, (uint8_t *)&begin + 1, sizeof(begin) - 1)) break;
                if (s_pb_state != PB_PREPARED) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                if (ring_size() == 0) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                uint32_t rate = begin.point_rate;
                if (rate == 0) rate = ED_DEFAULT_POINT_RATE;
                if (rate > ED_MAX_POINT_RATE) rate = ED_MAX_POINT_RATE;
                uint32_t applied = rate;
                rp_manager_set_rate_applied(rate, &applied);   /* status reports the rate really running */
                s_pb_state     = PB_PLAYING;
                s_output_armed = true;
                ESP_LOGI(TAG, "Begin @ %lu pps (running %lu)", (unsigned long)rate, (unsigned long)applied);
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Update point rate -------------------------------------- */
            /* Same layout as begin; changes the rate during playback without
             * the queue. Sent by clients such as Modulaser when the operator
             * changes the rate. */
            if (cmd == 'u') {
                ed_begin_cmd_t update;
                if (!tcp_recv_all(sock, (uint8_t *)&update + 1, sizeof(update) - 1)) break;
                if (s_pb_state != PB_PREPARED && s_pb_state != PB_PLAYING) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                uint32_t rate = update.point_rate;
                if (rate == 0) rate = ED_DEFAULT_POINT_RATE;
                if (rate > ED_MAX_POINT_RATE) rate = ED_MAX_POINT_RATE;
                uint32_t applied = rate;
                rp_manager_set_rate_applied(rate, &applied);
                ESP_LOGI(TAG, "Update @ %lu pps (running %lu)", (unsigned long)rate, (unsigned long)applied);
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Queue rate change -------------------------------------- */
            if (cmd == 'q') {
                ed_queue_rate_cmd_t qr;
                if (!tcp_recv_all(sock, (uint8_t *)&qr + 1, sizeof(qr) - 1)) break;
                if (s_pb_state != PB_PREPARED && s_pb_state != PB_PLAYING) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                if (rq_count >= RATE_QUEUE_CAP) {
                    if (!send_response(sock, RESP_FULL, cmd)) break;
                    continue;
                }
                rate_queue[rq_tail] = qr.point_rate;
                rq_tail = (rq_tail + 1) % RATE_QUEUE_CAP;
                rq_count++;
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Write data -------------------------------------------- */
            if (cmd == 'd') {
                ed_data_cmd_hdr_t hdr;
                if (!tcp_recv_all(sock, (uint8_t *)&hdr + 1, sizeof(hdr) - 1)) break;
                uint16_t npoints = hdr.npoints;

                if (s_pb_state != PB_PREPARED && s_pb_state != PB_PLAYING) {
                    /* Drain socket then NAK */
                    uint8_t discard[64];
                    size_t left = (size_t)npoints * sizeof(ed_point_t);
                    while (left > 0) {
                        size_t chunk = left < sizeof(discard) ? left : sizeof(discard);
                        if (!tcp_recv_all(sock, discard, chunk)) goto disconnect;
                        left -= chunk;
                    }
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }

                if (npoints > 0 && npoints > ring_free()) {
                    uint8_t discard[64];
                    size_t left = (size_t)npoints * sizeof(ed_point_t);
                    while (left > 0) {
                        size_t chunk = left < sizeof(discard) ? left : sizeof(discard);
                        if (!tcp_recv_all(sock, discard, chunk)) goto disconnect;
                        left -= chunk;
                    }
#if ED_DIAG
                    s_diag.full_naks++;
#endif
                    if (!send_response(sock, RESP_FULL, cmd)) break;
                    continue;
                }

                ed_point_t pt_chunk[128];
                uint16_t remaining = npoints;
                bool ok = true;

                while (remaining > 0) {
                    uint16_t chunk = remaining < 128 ? remaining : 128;
                    if (!tcp_recv_all(sock, pt_chunk, chunk * sizeof(ed_point_t))) {
                        ok = false;
                        break;
                    }
                    /* Apply queued rate changes from control bits */
                    for (uint16_t pi = 0; pi < chunk; pi++) {
                        if ((pt_chunk[pi].control & (1u << 15)) && rq_count > 0) {
                            uint32_t new_rate = rate_queue[rq_head];
                            rq_head  = (rq_head + 1) % RATE_QUEUE_CAP;
                            rq_count--;
                            if (new_rate > 0 && new_rate <= ED_MAX_POINT_RATE) {
                                uint32_t applied = new_rate;
                                rp_manager_set_rate_applied(new_rate, &applied);
                                ESP_LOGI(TAG, "Rate -> %lu pps (running %lu)", (unsigned long)new_rate, (unsigned long)applied);
                            }
                        }
                    }
                    uint32_t w = ring_write(pt_chunk, chunk);
                    if (w < chunk) {
                        ESP_LOGW(TAG, "Ring full mid-write (%u/%u)", w, chunk);
                        protocol_stream_stats_add_dropped(PROTOCOL_STREAM_ETHERDREAM, chunk - w);
                    }
                    remaining -= chunk;
                }

                if (!ok) break;
#if ED_DIAG
                uint32_t reported = ring_size() + s_rp_queued;
                s_diag.data_cmds++;
                s_diag.points += npoints;
                s_diag.payload_us += esp_timer_get_time() - cmd_us;
                s_diag.fullness_sum += reported;
                if (reported < s_diag.fullness_min) s_diag.fullness_min = reported;
                if (reported > s_diag.fullness_max) s_diag.fullness_max = reported;
#endif
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Stop -------------------------------------------------- */
            if (cmd == 's') {
                if (s_pb_state == PB_IDLE) {
                    if (!send_response(sock, RESP_INV, cmd)) break;
                    continue;
                }
                s_output_armed = false;
                s_pb_state     = PB_IDLE;
                s_point_count  = 0;
                ring_clear();
                stop_feed_task();
                ESP_LOGI(TAG, "Stop - Idle, ready for re-Prepare");
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Version query 'v' (0x76) ----------------------------- */
            /* Many Ether Dream clients send 'v' after Prepare to check
             * firmware capabilities.  Respond like a ping so they can
             * continue rather than stalling. */
            if (cmd == 'v') {
                if (!send_response(sock, RESP_ACK, cmd)) break;
                continue;
            }

            /* -- Unrecognized -> NAK (keep session alive) --------------- */
            /* Respond with RESP_INV so the client knows the command was
             * received but not understood, without triggering an E-Stop
             * that kills the session and forces a full reconnect. */
            ESP_LOGW(TAG, "Unknown cmd 0x%02X -> NAK", cmd);
            if (!send_response(sock, RESP_INV, cmd)) break;
        }
    } /* end scope for rate_queue locals */

disconnect:
    ESP_LOGI(TAG, "Closing connection (sock=%d)", sock);

    /* Full state reset - next client can Prepare without reboot */
    s_output_armed = false;
    s_pb_state     = PB_IDLE;
    stop_feed_task();
    ring_clear();
    s_le_state     = LE_READY;
    s_pb_flags     = 0;
    s_point_count  = 0;

    ESP_LOGI(TAG, "State reset - Idle/Ready for next client");

    s_client_sock = -1;
    close(sock);
}

#if BOARD_VERSION != 1
static void client_task(void *arg)
{
    handle_client((int)(intptr_t)arg);
    s_client_task_h = NULL;
    vTaskDelete(NULL);
}
#else
static void client_task(void *arg)
{
    (void)arg;

    while (true) {
        int sock = -1;
        if (xQueueReceive(s_client_sock_q, &sock, portMAX_DELAY) != pdTRUE || sock < 0) {
            continue;
        }

        s_client_active = true;
        handle_client(sock);
        s_client_active = false;
    }
}
#endif

/* -- TCP listener task ------------------------------------------------------ */
static void listener_task(void *arg)
{
    (void)arg;

    int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock < 0) {
        ESP_LOGE(TAG, "TCP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ED_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lsock, 1) != 0) {
        ESP_LOGE(TAG, "TCP bind/listen failed");
        close(lsock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Ether Dream TCP on port %d", ED_TCP_PORT);

    while (true) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int csock = accept(lsock, (struct sockaddr *)&caddr, &clen);
        if (csock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char ip[16];
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "TCP from %s", ip);

#if BOARD_VERSION == 1
        if (ilda_player_is_running()) {
            ESP_LOGI(TAG, "Stopping SD playback for Ether Dream client");
            esp_err_t stop_ret = ilda_player_stop_and_wait(1500);
            if (stop_ret != ESP_OK) {
                ESP_LOGW(TAG, "SD playback stop timed out: %s", esp_err_to_name(stop_ret));
            }
        }
        rp_manager_stop_test_stream();

        if (s_client_active || uxQueueMessagesWaiting(s_client_sock_q) > 0) {
            ESP_LOGW(TAG, "Kicking stale Ether Dream client");
            int old_sock = s_client_sock;
            if (old_sock >= 0) {
                shutdown(old_sock, SHUT_RDWR);
            }

            for (int i = 0; i < 100 &&
                    (s_client_active || uxQueueMessagesWaiting(s_client_sock_q) > 0); i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (s_client_active || uxQueueMessagesWaiting(s_client_sock_q) > 0) {
                ESP_LOGW(TAG, "Ether Dream worker still busy, rejecting new client");
                close(csock);
                continue;
            }
        }

        if (xQueueSend(s_client_sock_q, &csock, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Client worker queue full");
            close(csock);
            continue;
        }
#else
        if (s_client_task_h != NULL) {
            ESP_LOGW(TAG, "Rejecting second client");
            close(csock);
            continue;
        }

        BaseType_t ok = xTaskCreatePinnedToCore(
            client_task, "ed_client", STACK_CLIENT,
            (void *)(intptr_t)csock, ED_CLIENT_TASK_PRIORITY, &s_client_task_h, 0);

        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Client task create failed");
            close(csock);
            s_client_task_h = NULL;
        }
#endif
    }
}

/* -- UDP broadcast task ----------------------------------------------------- */

/* Tell clients the scanner limit, so a well-behaved one never asks for more. */
static uint32_t advertised_max_rate(void)
{
    uint32_t limit = rp_manager_get_rate_limit();
    return limit < ED_MAX_POINT_RATE ? limit : ED_MAX_POINT_RATE;
}

static void broadcast_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int bcast = 1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ED_UDP_BCAST_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGW(TAG, "UDP discovery bind failed on port %d; broadcast-only discovery", ED_UDP_BCAST_PORT);
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ED_UDP_BCAST_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    ESP_LOGI(TAG, "Ether Dream UDP broadcast on port %d", ED_UDP_BCAST_PORT);

    int64_t next_broadcast_us = 0;

    while (true) {
        ed_broadcast_t pkt;
        memcpy(pkt.mac_address, s_mac, 6);
        pkt.hw_revision    = 0x0002;
        pkt.sw_revision    = 0x0002;
        pkt.buffer_capacity = (uint16_t)ED_BUFFER_CAPACITY;
        pkt.max_point_rate  = advertised_max_rate();
        pkt.status          = make_status();

        int64_t now = esp_timer_get_time();
        if (now >= next_broadcast_us) {
            sendto(sock, &pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&dest, sizeof(dest));
            next_broadcast_us = now + 1000000LL;
        }

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, &pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n >= 0 && n != (int)sizeof(ed_broadcast_t)) {
            ed_broadcast_t reply;
            memcpy(reply.mac_address, s_mac, 6);
            reply.hw_revision    = 0x0002;
            reply.sw_revision    = 0x0002;
            reply.buffer_capacity = (uint16_t)ED_BUFFER_CAPACITY;
            reply.max_point_rate  = advertised_max_rate();
            reply.status          = make_status();
            sendto(sock, &reply, sizeof(reply), 0,
                   (struct sockaddr *)&from, sizeof(from));
        }
    }
}

/* -- Public init ------------------------------------------------------------ */
esp_err_t etherdream_protocol_start(void)
{
    ESP_LOGI(TAG, "Starting Ether Dream protocol");

    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_ETH));
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    s_ring = heap_caps_malloc(ED_RING_CAPACITY * sizeof(ed_point_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring)
        s_ring = heap_caps_malloc(ED_RING_CAPACITY * sizeof(ed_point_t),
                                  MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "Ring alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Ring: %u points (%u bytes)",
             ED_RING_CAPACITY,
             (unsigned)(ED_RING_CAPACITY * sizeof(ed_point_t)));

    BaseType_t ok;

#if BOARD_VERSION == 1
    s_client_sock_q = xQueueCreate(1, sizeof(int));
    if (!s_client_sock_q) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(
        client_task, "ed_client", STACK_CLIENT, NULL, ED_CLIENT_TASK_PRIORITY, &s_client_task_h, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
#endif

    ok = xTaskCreatePinnedToCore(
        broadcast_task, "ed_bcast", STACK_BROADCAST, NULL, 3, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ok = xTaskCreatePinnedToCore(
        listener_task, "ed_listen", STACK_LISTENER, NULL, ED_LISTENER_TASK_PRIORITY, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    return ESP_OK;
}
