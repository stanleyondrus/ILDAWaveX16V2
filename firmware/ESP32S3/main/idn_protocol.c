/*
 * idn_protocol.c
 *
 * IDN-Hello + IDN-RT + IDN-Stream implementation for ESP32-S3 + RP2350 Laser DAC.
 *
 * -- Transport ----------------------------------------------------------------
 * All traffic is UDP on port 7255.
 * Each UDP datagram = 4-byte IDN-Hello header + optional payload.
 *
 *   Byte 0: command
 *   Byte 1: flags  (bits 3-0 = client group)
 *   Bytes 2-3: sequence number (big-endian)
 *
 * -- IDN-RT streaming payload --------------------------------------------------
 * Payload = IDN-Stream channel message:
 *   Bytes 0-1: total size (big-endian)
 *   Byte  2:   CNL = CCLF(bit7) | channel_id(bits5-0)
 *   Byte  3:   chunk type
 *   Bytes 4-7: timestamp uss (big-endian)
 *
 * When CCLF=1 (and chunk_type < 0xC0), a channel configuration header follows:
 *   Byte  0:   SCWC  (service config word count x 4 = service config bytes)
 *   Byte  1:   CFL   bit0=Routing, bit1=Close
 *   Byte  2:   service ID
 *   Byte  3:   service mode
 *   Then SCWCx4 bytes of 16-bit BE tag words (the "tag dictionary")
 *
 * After the config header (or directly after the 8-byte message header when
 * CCLF=0), the data chunk follows.  For chunk type 0x01 (wave samples):
 *   Byte  0:   flags
 *   Bytes 1-3: duration uss (big-endian, 24-bit)
 *   Bytes 4..: sample array
 *
 * -- Tag dictionary ------------------------------------------------------------
 * Tags are 16-bit BE words structured as nibbles [CAT][SUB][ID][PRM]; they are
 * compiled by idn_decoder.c, which documents the supported subset. In short:
 * X/Y (0x4200/0x4210) with optional precision (0x4010), colours by wavelength
 * in nm (0x527E = 638 nm), intensity 0x5C10 and hint format 0x4100 scales are
 * decoded. Break, sample shutter (0x4101), wavelength prefixes and unknown
 * category/subcategory combinations reject the configuration.
 *
 * There is no default layout: wave data is played only on a routed channel
 * whose configuration compiled, and only as whole, exactly sized chunks of at
 * least 20 samples.
 *
 * -- Sample conversion -> RP int16 ---------------------------------------------
 * RP channels: ch[0]=X, ch[1]=Y, ch[2]=R, ch[3]=G, ch[4]=B, ch[5]=I, ch[6]=U1, ch[7]=U2
 * X/Y: left-aligned signed 16 bit (8-bit values get zero low bits)
 * Optical: left-aligned unsigned 16 bit, minus 32768; lines without a
 *          descriptor stay at -32768 (raw DAC 0)
 *
 * -- Buffer & feed -------------------------------------------------------------
 * SPSC ring of decoded rp_sample_t structs (same design as etherdream_protocol).
 * Feed task (core 1, priority 6) drains ring -> RP2350 with galvo-hold padding.
 *
 * -- Session / link ------------------------------------------------------------
 * idn_session.c owns connection, channel and route state. One session,
 * identified by client address and port. A channel plays only after Routing
 * with a configuration the decoder accepts; data on a closed channel is
 * refused (no auto-open). The link times out after 1 second without accepted
 * RT traffic (IDN-Hello section 7.2). Close, abort, timeout, group exclusion
 * and a local Stop cancel the feed task and wait for it to acknowledge
 * before the ring is reset.
 *
 * -- Timing and diagnostics (idn_timing.c, inside idn_session) -----------------
 * The session drops duplicate, late and stale RT packets (no link refresh)
 * and messages with earlier timestamps (BPErr), and requires each channel's
 * wave chunks to be exactly continuous (CTYErr). A discontinuity ends the
 * epoch: this file blanks across it (wave_epoch). The output rate is applied
 * once per output generation, just before START, from the samples queued;
 * chunks at another rate are then rejected (RGUErr) instead of retiming the
 * queue. ACK events are cleared only after the ACK was sent; latency and
 * link quality are 0 (unknown).
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

#include "app_config.h"
#include "rp_manager.h"
#include "idn_protocol.h"
#include "idn_decoder.h"
#include "idn_session.h"
#include "idn_timing.h"
#include "protocol_stream_stats.h"

_Static_assert(IDN_DECODER_CHANNELS == RP_SAMPLE_CHANNELS, "decoder and RP channel counts differ");

/* -- Tunables --------------------------------------------------------------- */
#define IDN_UDP_PORT            IDN_PROTOCOL_UDP_PORT
#define IDN_RX_MAX              1500u           /* larger datagrams are dropped, never truncated */
#define IDN_FEED_STOP_TIMEOUT_MS 1500u          /* feed exit includes the RP STOP round trip */
#define IDN_MIN_RATE_HZ         1000u           /* lowest wave rate accepted */
#define IDN_EPOCH_BLANK_SAMPLES 16u             /* blanked hold on each side of a discontinuity */
#define IDN_RING_CAPACITY       8192u           /* must be power of 2 */
#define IDN_FEED_FLUSH_US       100000LL        /* send a partial packet only after the stream idles this long */
#define IDN_FEED_PREBUFFER      3072u           /* samples queued on the RP before START (~100 ms at 30 kpps) */
#define IDN_MAX_CHANNELS        64
#define STACK_SERVER            8192u
#define STACK_FEED              4096u

#if BOARD_VERSION == 1
#define IDN_FEED_TASK_CORE     0
#define IDN_FEED_TASK_PRIORITY 5
#else
#define IDN_FEED_TASK_CORE     1
#define IDN_FEED_TASK_PRIORITY 6
#endif

/* -- IDN-Hello command codes ------------------------------------------------ */
#define IDNCMD_PING_REQ         0x08
#define IDNCMD_PING_RESP        0x09
#define IDNCMD_SCAN_REQ         0x10
#define IDNCMD_SCAN_RESP        0x11
#define IDNCMD_SVCMAP_REQ       0x12
#define IDNCMD_SVCMAP_RESP      0x13
#define IDNCMD_RT_ACK           0x47

/* -- IDN service types ------------------------------------------------------ */
#define IDNSVC_LASER            0x80

/* -- Packed wire structures ------------------------------------------------- */
#pragma pack(push, 1)

/* Scan response: 40 bytes (IDNHDR_SCAN_RESPONSE) */
typedef struct {
    uint8_t struct_size;
    uint8_t protocol_version;
    uint8_t status;
    uint8_t pad;
    uint8_t unit_id[16];    /* [0]=len [1]=cat(0x01=EUI-48) [2-7]=MAC [8-15]=0 */
    uint8_t host_name[20];
} idn_scan_resp_t;

/* Service map response header: 4 bytes */
typedef struct {
    uint8_t struct_size;
    uint8_t entry_size;
    uint8_t relay_count;
    uint8_t service_count;
} idn_svcmap_resp_t;

/* Service map entry: 24 bytes */
typedef struct {
    uint8_t service_id;
    uint8_t service_type;
    uint8_t flags;
    uint8_t relay_number;
    uint8_t name[20];
} idn_svcmap_entry_t;

#pragma pack(pop)

/* -- Helpers: big-endian read/write ---------------------------------------- */
static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* -- Channel decoders (route and channel state live in idn_session) --------- */
typedef struct {
    idn_decoder_layout_t layout;        /* invalid until a configuration compiles */
    idn_decoder_reason_t last_reject;   /* logged once per change */
} idn_channel_decoder_t;

_Static_assert(IDN_MAX_CHANNELS == IDN_SESSION_CHANNELS, "decoder and session channel counts differ");

static idn_channel_decoder_t s_decoders[IDN_MAX_CHANNELS];

/* -- SPSC ring buffer (decoded RP samples) --------------------------------- */
typedef struct {
    int16_t ch[RP_SAMPLE_CHANNELS];
} rp_sample_t;

static rp_sample_t       *s_ring      = NULL;
static volatile uint32_t  s_ring_head = 0;
static volatile uint32_t  s_ring_tail = 0;

static inline uint32_t ring_size(void)
{
    uint32_t h = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE);
    return (t - h + IDN_RING_CAPACITY * 2u) % (IDN_RING_CAPACITY * 2u);
}

static inline uint32_t ring_free(void)
{
    return IDN_RING_CAPACITY - 1u - ring_size();
}

static uint32_t ring_write(const rp_sample_t *pts, uint32_t n)
{
    uint32_t free_space = ring_free();
    if (n > free_space) n = free_space;
    uint32_t tail = s_ring_tail;
    uint32_t mask = IDN_RING_CAPACITY - 1u;
    uint32_t wrap = IDN_RING_CAPACITY * 2u;
    for (uint32_t i = 0; i < n; i++)
        s_ring[(tail + i) & mask] = pts[i];
    __atomic_store_n(&s_ring_tail, (tail + n) % wrap, __ATOMIC_RELEASE);
    return n;
}

static uint32_t ring_read(rp_sample_t *out, uint32_t n)
{
    uint32_t avail = ring_size();
    if (n > avail) n = avail;
    uint32_t head = s_ring_head;
    uint32_t mask = IDN_RING_CAPACITY - 1u;
    uint32_t wrap = IDN_RING_CAPACITY * 2u;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s_ring[(head + i) & mask];
    __atomic_store_n(&s_ring_head, (head + n) % wrap, __ATOMIC_RELEASE);
    return n;
}

static void ring_clear(void) { s_ring_head = 0; s_ring_tail = 0; }

static const char *TAG = "idn";

/*
   Output rate (plan 03). The server task proposes the rate of the samples
   queued for the current output generation; the feed applies it once, just
   before START, and it stays locked for that generation. 0 = none.
*/
static volatile uint32_t s_proposed_rate = 0;
static volatile uint32_t s_locked_rate   = 0;
static uint32_t          s_rp_max_rate   = 0;   /* RP's own limit when the output was taken; 0 = none */

/* Set once the feed has handed samples to the RP in this output run. No
   completion point is known, so it stays set until the feed stops. */
static volatile bool s_device_may_hold = false;

/* Server task: last sample queued in this generation, for blanked holds. */
static rp_sample_t s_last_queued;
static bool        s_have_last_queued = false;
static bool        s_lead_in_pending  = false;  /* next chunk starts a new epoch */

static idn_protocol_counters_t s_counters;

#define COUNT(field, n) __atomic_add_fetch(&s_counters.field, (uint32_t)(n), __ATOMIC_RELAXED)

static idn_rate_policy_t rate_policy(void)
{
    idn_rate_policy_t p = {
        .min_hz        = IDN_MIN_RATE_HZ,
        .limit_hz      = rp_manager_get_rate_limit(),
        .resolution_us = 1,
    };
    if (s_rp_max_rate != 0 && s_rp_max_rate < p.limit_hz) {
        p.limit_hz = s_rp_max_rate;
    }
    return p;
}

/*
   Apply the proposed rate, then START. Never called at receive time: a
   RATE command retimes every sample already queued on the RP.
*/
static bool feed_output_start(void)
{
    uint32_t rate = __atomic_load_n(&s_proposed_rate, __ATOMIC_ACQUIRE);
    uint32_t applied = rate;

    if (rate == 0 || rp_manager_set_rate_applied(rate, &applied) != ESP_OK) {
        return false;
    }
    if (applied != rate) {
        ESP_LOGW(TAG, "Asked %lu pps, RP runs %lu; chunks at %lu will be rejected",
                 (unsigned long)rate, (unsigned long)applied, (unsigned long)rate);
    }
    __atomic_store_n(&s_locked_rate, applied, __ATOMIC_RELEASE);
    __atomic_store_n(&s_counters.selected_rate, applied, __ATOMIC_RELAXED);

    rp_manager_output_start();
    return true;
}

/* -- Module globals --------------------------------------------------------- */

static uint8_t s_mac[6];
static int     s_sock = -1;

/* Session: touched only by the server task */
static idn_session_t s_session;

/* Local Stop requests, served by the server task */
static volatile bool     s_server_running = false;
static volatile bool     s_cancel_requested = false;
static esp_err_t         s_cancel_result = ESP_OK;
static SemaphoreHandle_t s_cancel_done = NULL;

/*
   Feed. s_feed_alive is the server task's view: set when it creates the
   task, cleared only once the task has signalled s_feed_done on its way out.
*/
static volatile bool     s_feed_cancel = false;
static bool              s_feed_alive  = false;
static uint32_t          s_output_gen  = 0;
static SemaphoreHandle_t s_feed_wake   = NULL;   /* interrupts the feed's idle waits */
static SemaphoreHandle_t s_feed_done   = NULL;

/* Feed buffers (static = no stack pressure) */
static int16_t     s_feed_packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];
static rp_sample_t s_feed_stage[RP_STREAM_SAMPLES_PER_PACKET];

/* -- Decoder ---------------------------------------------------------------- */
static idn_decoder_caps_t s_decoder_caps;

static void log_decoder_reject(uint8_t ch_id, idn_channel_decoder_t *ch, const char *what,
                               idn_decoder_result_t r)
{
    if (ch->last_reject == r.reason) return;
    ch->last_reject = r.reason;
    ESP_LOGW(TAG, "Channel %u %s rejected: %s (%s), tag 0x%04X at word %u",
             ch_id, what, idn_decoder_status_name(r.status), idn_decoder_reason_name(r.reason),
             r.tag, r.tag_index);
}

/* -- Feed task -------------------------------------------------------------- */
static inline bool feed_cancelled(void)
{
    return __atomic_load_n(&s_feed_cancel, __ATOMIC_ACQUIRE);
}

/* Idle wait that a cancellation cuts short. */
static inline void feed_wait(TickType_t ticks)
{
    xSemaphoreTake(s_feed_wake, ticks > 0 ? ticks : 1);
}

static void feed_task(void *arg)
{
    (void)arg;

    /* Initial "last known" point: blanked, at origin */
    rp_sample_t last;
    memset(&last, 0, sizeof(last));
    last.ch[2] = last.ch[3] = last.ch[4] = last.ch[5] = -32768;

    bool output_started = false;
    uint32_t queued_before_start = 0;
    uint32_t last_avail = 0;
    int64_t last_growth_us = esp_timer_get_time();

    ESP_LOGI(TAG, "IDN feed task started");

    while (!feed_cancelled()) {
        /*
           Send only full packets while the stream is flowing. Padding a
           partial packet splices filler into the drawing, and the RP buffer
           is deep enough to wait for more points. A partial packet is sent
           only once the stream has gone idle, to flush its tail; an empty
           ring sends nothing and the RP blanks on underrun.
        */
        uint32_t avail   = ring_size();
        uint32_t to_read = (uint32_t)RP_STREAM_SAMPLES_PER_PACKET;

        if (avail < (uint32_t)RP_STREAM_SAMPLES_PER_PACKET) {
            int64_t now_us = esp_timer_get_time();

            if (avail != last_avail) {
                last_avail = avail;
                last_growth_us = now_us;
            }

            bool idle = now_us - last_growth_us >= IDN_FEED_FLUSH_US;

            if (avail == 0 || !idle) {
                /* A stream that idles on a packet boundary has no tail to flush; start what is queued. */
                if (avail == 0 && idle && !output_started && queued_before_start > 0 && !feed_cancelled() &&
                    feed_output_start()) {
                    output_started = true;
                    ESP_LOGI(TAG, "IDN RP output started");
                }
                feed_wait(1);
                continue;
            }

            to_read = avail;
        }

        uint32_t got = ring_read(s_feed_stage, to_read);
        last_avail = ring_size();
        last_growth_us = esp_timer_get_time();

        for (uint32_t i = 0; i < got; i++) {
            memcpy(s_feed_packet[i], s_feed_stage[i].ch, sizeof(s_feed_packet[i]));
            last = s_feed_stage[i];
        }

        /* Pad a flushed tail by holding position with the laser blanked. */
        for (uint32_t i = got; i < (uint32_t)RP_STREAM_SAMPLES_PER_PACKET; i++) {
            s_feed_packet[i][0] = last.ch[0];
            s_feed_packet[i][1] = last.ch[1];
            for (int ch = 2; ch < RP_SAMPLE_CHANNELS; ch++) {
                s_feed_packet[i][ch] = -32768;
            }
        }

        esp_err_t err = rp_manager_send_samples_packet(s_feed_packet);
        if (err == ESP_OK) {
            queued_before_start += got;
            s_device_may_hold = true;

            /* Start once a short cushion is queued, or when a short stream is flushed. */
            if (!output_started && !feed_cancelled() &&
                (queued_before_start >= IDN_FEED_PREBUFFER || got < (uint32_t)RP_STREAM_SAMPLES_PER_PACKET) &&
                feed_output_start()) {
                output_started = true;
                ESP_LOGI(TAG, "IDN RP output started");
            }
            protocol_stream_stats_add_samples(PROTOCOL_STREAM_IDN, got);
        } else {
            ESP_LOGW(TAG, "send_samples_packet: %s", esp_err_to_name(err));
            feed_wait(pdMS_TO_TICKS(2));
        }

        taskYIELD();
    }

    /* Release only a START this feed made; otherwise drop its prebuffer. */
    if (output_started)
        rp_manager_output_stop();
    else if (queued_before_start > 0)
        rp_manager_output_discard_unstarted();
    s_device_may_hold = false;
    ESP_LOGI(TAG, "IDN feed task exiting");

    /* Last touch of shared state: the server may reset the ring after this. */
    xSemaphoreGive(s_feed_done);
    vTaskDelete(NULL);
}

static esp_err_t feed_start(void)
{
    /* Never rearm or overlap a task that has not acknowledged its exit. */
    if (s_feed_alive) {
        return ESP_ERR_INVALID_STATE;
    }

    __atomic_store_n(&s_feed_cancel, false, __ATOMIC_RELEASE);
    xSemaphoreTake(s_feed_wake, 0);
    xSemaphoreTake(s_feed_done, 0);

    BaseType_t ok = xTaskCreatePinnedToCore(
        feed_task, "idn_feed", STACK_FEED, NULL, IDN_FEED_TASK_PRIORITY, NULL, IDN_FEED_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_feed_alive = true;
    return ESP_OK;
}

/*
   Cancel the feed and wait for it to exit. Its exit path releases the RP
   output (STOP, or CLEAR of an unstarted prebuffer) before acknowledging.
   false: still running after the deadline; the ring must not be touched.
*/
static bool feed_stop_wait(uint32_t timeout_ms)
{
    if (!s_feed_alive) {
        return true;
    }

    __atomic_store_n(&s_feed_cancel, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_feed_wake);

    if (xSemaphoreTake(s_feed_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    s_feed_alive = false;
    return true;
}

static bool feed_poll_stopped(void)
{
    if (s_feed_alive && xSemaphoreTake(s_feed_done, 0) == pdTRUE) {
        s_feed_alive = false;
    }
    return !s_feed_alive;
}

static void output_reset(void)
{
    ring_clear();
    __atomic_store_n(&s_proposed_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_locked_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_counters.selected_rate, 0, __ATOMIC_RELAXED);
    s_have_last_queued = false;
    s_lead_in_pending = false;
}

/* -- Send helpers ----------------------------------------------------------- */

/*
 * Send an IDN-Hello packet.  payload may be NULL if payload_len == 0.
 * Uses a stack buffer for small payloads (management responses).
 * For large payloads (ping echo) we allocate on heap.
 */
static bool send_pkt(const struct sockaddr_in *dest,
                     uint8_t cmd, uint8_t flags, uint16_t seq,
                     const void *payload, size_t payload_len)
{
    size_t total = 4 + payload_len;
    uint8_t *buf;
    uint8_t  sbuf[128];
    bool     heap = (total > sizeof(sbuf));

    if (heap) {
        buf = malloc(total);
        if (!buf) return false;
    } else {
        buf = sbuf;
    }

    buf[0] = cmd;
    buf[1] = flags;
    wr16(&buf[2], seq);
    if (payload && payload_len)
        memcpy(&buf[4], payload, payload_len);

    int sent = sendto(s_sock, buf, total, 0,
                      (const struct sockaddr *)dest, sizeof(*dest));

    if (heap) free(buf);
    return sent == (int)total;
}

/*
   Send the ACK the session built. The session sets SOCnl; SNMsg and DOBuf
   come from the queues this file owns. DOBuf is conservative: no RP
   completion point is known yet. Latency and link quality stay 0
   (unknown). Latched events are cleared only once the ACK has gone out.
*/
static void send_ack(const struct sockaddr_in *dest,
                     uint8_t flags, uint16_t seq, const idn_rt_outcome_t *out)
{
    idn_ack_fields_t f;
    memset(&f, 0, sizeof(f));
    f.result_code          = out->ack.result;
    f.input_event_flags    = out->ack.input_events;
    f.pipeline_event_flags = out->ack.pipeline_events;
    f.status_flags         = out->ack.status;
    if (out->latched)
        f.status_flags |= idn_ack_status_flags(false, ring_size() > 0, s_device_may_hold);

    uint8_t ack[IDN_ACK_STRUCT_SIZE];
    idn_ack_serialize(&f, ack);
    if (send_pkt(dest, IDNCMD_RT_ACK, flags, seq, ack, sizeof(ack))) {
        idn_session_ack_sent(&s_session, out);
        COUNT(acks_sent, 1);
    }
}

/* -- Discovery handlers ----------------------------------------------------- */
static void handle_scan_req(const struct sockaddr_in *from,
                             uint8_t flags, uint16_t seq)
{
    /* One session: OCPD whenever it is held (or faulted), whoever asks. */
    idn_scan_state_t st = {
        .rt_supported    = true,
        .sessions_total  = 1,
        .sessions_in_use = idn_session_occupied(&s_session) ? 1u : 0u,
        .group_excluded  = idn_session_group_excluded(&s_session, flags & 0x0F),
    };

    idn_scan_resp_t r;
    memset(&r, 0, sizeof(r));
    r.struct_size       = sizeof(r);
    r.protocol_version  = 0x01;     /* IDN-Hello v1: major=0, minor=1 */
    r.status            = idn_scan_status(&st);

    /* Unit ID: EUI-48 from Ethernet MAC */
    r.unit_id[0] = 7;       /* 1 byte category + 6 bytes MAC */
    r.unit_id[1] = 0x01;    /* category: EUI-48 */
    memcpy(&r.unit_id[2], s_mac, 6);

    strncpy((char *)r.host_name, "ILDAWaveX16", sizeof(r.host_name) - 1);

    send_pkt(from, IDNCMD_SCAN_RESP, flags, seq, &r, sizeof(r));
}

static void handle_svcmap_req(const struct sockaddr_in *from,
                               uint8_t flags, uint16_t seq)
{
    uint8_t buf[sizeof(idn_svcmap_resp_t) + sizeof(idn_svcmap_entry_t)];

    idn_svcmap_resp_t  *hdr   = (idn_svcmap_resp_t *)buf;
    idn_svcmap_entry_t *entry = (idn_svcmap_entry_t *)(buf + sizeof(*hdr));

    hdr->struct_size   = sizeof(*hdr);
    hdr->entry_size    = sizeof(*entry);
    hdr->relay_count   = 0;
    hdr->service_count = 1;

    memset(entry, 0, sizeof(*entry));
    entry->service_id   = 1;
    entry->service_type = IDNSVC_LASER;
    entry->flags        = 0x01;    /* DSID: this is the default service */
    entry->relay_number = 0;
    strncpy((char *)entry->name, "laser", sizeof(entry->name) - 1);

    send_pkt(from, IDNCMD_SVCMAP_RESP, flags, seq, buf, sizeof(buf));
}

/* -- Wave chunk processing -------------------------------------------------- */
static uint32_t ring_write_blank(const rp_sample_t *at, uint32_t n)
{
    rp_sample_t blank[IDN_EPOCH_BLANK_SAMPLES];

    if (n > IDN_EPOCH_BLANK_SAMPLES) n = IDN_EPOCH_BLANK_SAMPLES;
    for (uint32_t i = 0; i < n; i++) {
        for (int c = 0; c < RP_SAMPLE_CHANNELS; c++)
            blank[i].ch[c] = -32768;    /* every optical line off (raw DAC 0) */
        blank[i].ch[0] = at->ch[0];
        blank[i].ch[1] = at->ch[1];
    }

    uint32_t w = ring_write(blank, n);
    if (w > 0) {
        s_last_queued = blank[w - 1];
        COUNT(recovery_samples, w);
    }
    return w;
}

/*
   Decode a wave chunk the session has accepted for a routed channel. The
   whole chunk was validated by ops_wave_check() (header, duration, exact
   stride, >= 20 samples) and by the session's continuity and rate checks;
   it is queued whole or not at all.
*/
static idn_chunk_result_t process_wave_chunk(uint8_t ch_id,
                                             const uint8_t *data, uint16_t data_len)
{
    idn_channel_decoder_t *ch = &s_decoders[ch_id];

    uint32_t nsamples = 0, duration_us = 0;
    idn_decoder_result_t r = idn_decoder_check_wave_chunk(&ch->layout, data, data_len,
                                                          &nsamples, &duration_us);
    if (r.status != IDN_DECODER_OK) {
        return IDN_CHUNK_INTERNAL;  /* checked just before; cannot differ */
    }

    const uint8_t *samples = data + IDN_DECODER_WAVE_HEADER_BYTES;
    uint8_t        stride  = ch->layout.stride;
    bool           lead_in = s_lead_in_pending && s_have_last_queued;
    uint32_t       needed  = nsamples + (lead_in ? IDN_EPOCH_BLANK_SAMPLES : 0u);

    /* Queue the whole chunk or none of it; this task is the only producer. */
    if (needed > ring_free()) {
        protocol_stream_stats_add_dropped(PROTOCOL_STREAM_IDN, nsamples);
        return IDN_CHUNK_DEVICE_OVERRUN;
    }

    rp_sample_t burst[64];

    /* A new epoch after earlier output: move there blanked first. */
    if (lead_in) {
        idn_decoder_decode(&ch->layout, samples, 1, &burst[0].ch);
        ring_write_blank(&burst[0], IDN_EPOCH_BLANK_SAMPLES);
    }
    s_lead_in_pending = false;

    for (uint32_t s = 0; s < nsamples; ) {
        uint32_t n = nsamples - s;
        if (n > 64) n = 64;

        for (uint32_t j = 0; j < n; j++)
            idn_decoder_decode(&ch->layout, samples + (size_t)(s + j) * stride, 1, &burst[j].ch);

        uint32_t w = ring_write(burst, n);
        if (w > 0) {
            s_last_queued = burst[w - 1];
            s_have_last_queued = true;
        }
        if (w < n) {
            /* Ring full: the rest of this chunk is lost. Count it. */
            protocol_stream_stats_add_dropped(PROTOCOL_STREAM_IDN, nsamples - s - w);
            return IDN_CHUNK_DEVICE_OVERRUN;
        }
        s += n;
    }
    COUNT(queued_samples, nsamples);

    /* Propose the epoch's rate including this chunk; the feed applies it
       once before START. The session commits the same chunk after this. */
    idn_rate_epoch_t epoch = s_session.ch[ch_id].rate;
    idn_rate_policy_t policy = rate_policy();
    idn_rate_commit(&epoch, nsamples, duration_us);
    __atomic_store_n(&s_proposed_rate, idn_rate_select(&policy, &epoch), __ATOMIC_RELEASE);

    uint64_t mhz = idn_rate_requested_mhz(&epoch);
    __atomic_store_n(&s_counters.requested_rate_mhz,
                     mhz > UINT32_MAX ? UINT32_MAX : (uint32_t)mhz, __ATOMIC_RELAXED);

    return IDN_CHUNK_OK;
}

/* -- Session actions (idn_session_ops_t) ------------------------------------ */
static const char *stop_reason_name(idn_stop_reason_t reason)
{
    switch (reason) {
        case IDN_STOP_CHANNEL_CLOSE: return "channel close";
        case IDN_STOP_ROUTE_CHANGE:  return "route change";
        case IDN_STOP_GRACEFUL:      return "connection close";
        case IDN_STOP_ABORT:         return "abort";
        case IDN_STOP_TIMEOUT:       return "link timeout";
        case IDN_STOP_EXCLUDED:      return "group excluded";
        case IDN_STOP_CANCEL:        return "local stop";
        default:                     return "?";
    }
}

/*
   Compile through idn_decoder (plan 01). There is no default layout: an
   absent or rejected dictionary leaves the channel without a decoder, and
   the session voids the channel. idn_decoder_compile() resets the layout on
   any error, so a failed replacement never falls back to the old one.
*/
static idn_cfg_result_t ops_config_compile(void *ctx, uint8_t ch_id, uint8_t mode,
                                           const uint8_t *cfg, uint16_t cfg_len)
{
    (void)ctx;
    (void)mode;

    idn_channel_decoder_t *ch = &s_decoders[ch_id];
    idn_decoder_result_t r = idn_decoder_compile(cfg, cfg_len, &s_decoder_caps, &ch->layout);

    switch (r.status) {
        case IDN_DECODER_OK:
            if (ch->last_reject != IDN_DECODER_REASON_NONE)
                ESP_LOGI(TAG, "Channel %u configuration accepted", ch_id);
            ch->last_reject = IDN_DECODER_REASON_NONE;
            ESP_LOGI(TAG, "Channel %u routed, %u bytes/sample", ch_id, ch->layout.stride);
            return IDN_CFG_OK;
        case IDN_DECODER_ERR_UNSUPPORTED:
            log_decoder_reject(ch_id, ch, "configuration", r);
            return IDN_CFG_UNSUPPORTED;
        case IDN_DECODER_ERR_RESOURCE:
            log_decoder_reject(ch_id, ch, "configuration", r);
            return IDN_CFG_NO_RESOURCES;
        default:
            log_decoder_reject(ch_id, ch, "configuration", r);
            return IDN_CFG_INVALID;
    }
}

static void ops_config_discard(void *ctx, uint8_t ch_id)
{
    (void)ctx;
    memset(&s_decoders[ch_id].layout, 0, sizeof(s_decoders[ch_id].layout));
}

static idn_chunk_result_t ops_wave_check(void *ctx, uint8_t ch_id,
                                         const uint8_t *chunk, uint16_t chunk_len,
                                         uint32_t *samples)
{
    (void)ctx;

    idn_channel_decoder_t *ch = &s_decoders[ch_id];
    uint32_t nsamples = 0, duration_us = 0;
    idn_decoder_result_t r = idn_decoder_check_wave_chunk(&ch->layout, chunk, chunk_len,
                                                          &nsamples, &duration_us);
    if (r.status == IDN_DECODER_OK) {
        *samples = nsamples;
        return IDN_CHUNK_OK;
    }

    log_decoder_reject(ch_id, ch, "wave chunk", r);

    switch (r.reason) {
        case IDN_DECODER_REASON_CHUNK_HEADER:
        case IDN_DECODER_REASON_TOO_FEW_SAMPLES:
            return IDN_CHUNK_TOO_SHORT;
        case IDN_DECODER_REASON_INVALID_LAYOUT:
            return IDN_CHUNK_INTERNAL;
        default:
            return r.status == IDN_DECODER_ERR_UNSUPPORTED ? IDN_CHUNK_UNSUPPORTED
                                                           : IDN_CHUNK_PAYLOAD_INVALID;
    }
}

static bool ops_output_acquire(void *ctx, uint32_t gen)
{
    (void)ctx;

    if (s_feed_alive) {
        return false;   /* the last feed never acknowledged its stop */
    }

    output_reset();
    s_output_gen = gen;

    if (feed_start() != ESP_OK) {
        ESP_LOGE(TAG, "IDN feed task start failed");
        return false;
    }

    /* No rate is set here: the feed applies one before START (plan 03). */
    ESP_LOGI(TAG, "IDN output %lu acquired", (unsigned long)gen);
    return true;
}

static idn_chunk_result_t ops_wave_enqueue(void *ctx, uint8_t ch_id, uint32_t gen,
                                           const uint8_t *data, uint16_t data_len)
{
    (void)ctx;

    if (gen != s_output_gen || !s_feed_alive) {
        return IDN_CHUNK_INTERNAL;
    }
    return process_wave_chunk(ch_id, data, data_len);
}

static bool ops_output_release(void *ctx, uint32_t gen, idn_stop_reason_t reason)
{
    (void)ctx;

    if (!feed_stop_wait(IDN_FEED_STOP_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "IDN output %lu: feed did not stop within %u ms (%s); IDN unavailable until it does",
                 (unsigned long)gen, (unsigned)IDN_FEED_STOP_TIMEOUT_MS, stop_reason_name(reason));
        return false;
    }

    output_reset();
    ESP_LOGI(TAG, "IDN output %lu released (%s)", (unsigned long)gen, stop_reason_name(reason));
    return true;
}

static bool ops_output_quiescent(void *ctx)
{
    (void)ctx;

    if (!feed_poll_stopped()) {
        return false;
    }
    output_reset();
    return true;
}

static void ops_rate_limits(void *ctx, uint32_t gen, idn_rate_policy_t *policy,
                            uint32_t *locked_hz)
{
    (void)ctx;

    if (gen == 0) {
        /* No output held: refresh the RP's own limit before a new run. */
        rp_status_t rp;
        rp_manager_get_status(&rp);
        s_rp_max_rate = rp.remote_max_rate;
    }

    *policy = rate_policy();
    *locked_hz = (gen != 0 && gen == s_output_gen)
                     ? __atomic_load_n(&s_locked_rate, __ATOMIC_ACQUIRE) : 0;
}

/* A continuous epoch ends (blank hold at the last point) or starts (blanked
   lead-in at its first point, if this output already queued samples). */
static void ops_wave_epoch(void *ctx, uint8_t ch_id, uint32_t gen, bool start)
{
    (void)ctx;
    (void)ch_id;

    if (gen != s_output_gen || !s_feed_alive) {
        return;
    }
    if (start) {
        s_lead_in_pending = true;
    } else if (s_have_last_queued) {
        ESP_LOGW(TAG, "Channel %u discontinuity; blanked", ch_id);
        ring_write_blank(&s_last_queued, IDN_EPOCH_BLANK_SAMPLES);
    }
}

static const idn_session_ops_t s_session_ops = {
    .config_compile   = ops_config_compile,
    .config_discard   = ops_config_discard,
    .wave_check       = ops_wave_check,
    .output_acquire   = ops_output_acquire,
    .wave_enqueue     = ops_wave_enqueue,
    .output_release   = ops_output_release,
    .output_quiescent = ops_output_quiescent,
    .rate_limits      = ops_rate_limits,
    .wave_epoch       = ops_wave_epoch,
};

/* -- IDN-RT packet handler -------------------------------------------------- */
static void handle_rt_packet(const struct sockaddr_in *from,
                              uint8_t cmd, uint8_t flags, uint16_t seq,
                              const uint8_t *payload, uint16_t payload_len)
{
    /* The socket is wildcard-bound: the local address is not known (0). */
    idn_endpoint_t ep = {
        .peer_addr  = from->sin_addr.s_addr,
        .peer_port  = from->sin_port,
        .local_addr = 0,
    };
    idn_rt_outcome_t out;

    idn_session_rt(&s_session, esp_timer_get_time(), &ep, cmd, flags, seq,
                   payload, payload_len, &out);

    if (out.connection_opened)
        ESP_LOGI(TAG, "IDN session from %s:%d",
                 inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    if (out.connection_closed)
        ESP_LOGI(TAG, "IDN session closed (%s)",
                 cmd == IDN_CMD_RT_ABORT ? "abort" : "close");

    if (out.send_ack) send_ack(from, flags, seq, &out);
}

static void handle_group_req(const struct sockaddr_in *from, uint8_t flags, uint16_t seq,
                             const uint8_t *payload, uint16_t payload_len)
{
    uint8_t resp[IDN_GROUP_RESP_BYTES];
    bool was_active = s_session.state == IDN_SESSION_ACTIVE;

    idn_session_group_request(&s_session, payload, payload_len, resp);

    if (resp[1] == IDN_GROUP_OK && payload_len >= 2 && payload[1] == 0x02)
        ESP_LOGI(TAG, "IDN client group mask 0x%02X%02X", resp[2], resp[3]);
    if (was_active && s_session.state != IDN_SESSION_ACTIVE)
        ESP_LOGI(TAG, "IDN session closed (client group excluded)");

    send_pkt(from, IDN_CMD_GROUP_RESP, flags, seq, resp, sizeof(resp));
}

static void session_tick(void)
{
    idn_session_state_t before = s_session.state;

    idn_session_tick(&s_session, esp_timer_get_time());

    if (before == IDN_SESSION_ACTIVE && s_session.state != IDN_SESSION_ACTIVE)
        ESP_LOGW(TAG, "IDN link timeout");
    if (before == IDN_SESSION_FAULTED && s_session.state == IDN_SESSION_FREE)
        ESP_LOGI(TAG, "IDN feed stopped late; session available again");

    if (__atomic_exchange_n(&s_cancel_requested, false, __ATOMIC_ACQ_REL)) {
        if (s_session.state == IDN_SESSION_ACTIVE)
            ESP_LOGI(TAG, "IDN session closed (local stop)");
        idn_session_cancel(&s_session, IDN_STOP_CANCEL);
        s_cancel_result = s_session.output_fault ? ESP_ERR_TIMEOUT : ESP_OK;
        xSemaphoreGive(s_cancel_done);
    }
}

/* -- Main server task ------------------------------------------------------- */
static void server_task(void *arg)
{
    (void)arg;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        vTaskDelete(NULL);
        return;
    }

    int v = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &v, sizeof(v));
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));

    /* 100 ms recv timeout so we can poll for link timeout */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(IDN_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed on port %d", IDN_UDP_PORT);
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "IDN-Hello server on UDP port %d", IDN_UDP_PORT);

    /*
     * One octet more than the largest accepted datagram, so a longer one
     * (which recvfrom truncates silently) can be recognised and dropped
     * instead of parsed or echoed in part. Static to keep it off the stack.
     */
    static uint8_t rxbuf[IDN_RX_MAX + 1];

    s_server_running = true;

    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(s_sock, rxbuf, sizeof(rxbuf), 0,
                         (struct sockaddr *)&from, &from_len);

        /* Link timeout and Stop requests on every loop iteration, even on recv timeout */
        session_tick();

        if (n < 4) continue;    /* too short or EAGAIN from SO_RCVTIMEO */
        COUNT(rx_datagrams, 1);
        if (n > (int)IDN_RX_MAX) {
            ESP_LOGD(TAG, "Datagram over %u bytes dropped", (unsigned)IDN_RX_MAX);
            continue;
        }

        uint8_t  cmd   = rxbuf[0];
        uint8_t  flags = rxbuf[1];
        uint16_t seq   = rd16(&rxbuf[2]);

        const uint8_t *payload     = rxbuf + 4;
        uint16_t       payload_len = (uint16_t)(n - 4);

        switch (cmd) {
            case IDNCMD_PING_REQ:
                /* Echo payload verbatim */
                send_pkt(&from, IDNCMD_PING_RESP, flags, seq,
                         payload, payload_len);
                break;

            case IDN_CMD_GROUP_REQ:
                handle_group_req(&from, flags, seq, payload, payload_len);
                break;

            case IDNCMD_SCAN_REQ:
                handle_scan_req(&from, flags, seq);
                break;

            case IDNCMD_SVCMAP_REQ:
                handle_svcmap_req(&from, flags, seq);
                break;

            case IDN_CMD_RT_CHANNEL:
            case IDN_CMD_RT_CHANNEL_ACK:
            case IDN_CMD_RT_CLOSE:
            case IDN_CMD_RT_CLOSE_ACK:
            case IDN_CMD_RT_ABORT:
                handle_rt_packet(&from, cmd, flags, seq, payload, payload_len);
                break;

            default:
                /* Silently discard unknown commands */
                break;
        }
    }
}

/* -- Public API ------------------------------------------------------------- */
void idn_protocol_get_counters(idn_protocol_counters_t *out)
{
    if (!out) return;

    /* Each field is a separate aligned 32-bit load; the session's counters
       are written by the server task only. */
    const idn_session_counters_t *sc = &s_session.counters;
#define LOAD(p) __atomic_load_n(&(p), __ATOMIC_RELAXED)
    out->rx_datagrams       = LOAD(s_counters.rx_datagrams);
    out->rt_accepted        = LOAD(sc->rt_accepted);
    out->malformed          = LOAD(sc->malformed);
    out->seq_duplicates     = LOAD(sc->seq_duplicates);
    out->seq_late           = LOAD(sc->seq_late);
    out->seq_stale          = LOAD(sc->seq_stale);
    out->seq_missing        = LOAD(sc->seq_missing);
    out->seq_resyncs        = LOAD(sc->seq_resyncs);
    out->ts_before          = LOAD(sc->ts_before);
    out->ts_resyncs         = LOAD(sc->ts_resyncs);
    out->continuity_errors  = LOAD(sc->continuity_errors);
    out->rate_rejects       = LOAD(sc->rate_rejects);
    out->chunk_rejects      = LOAD(sc->chunk_rejects);
    out->queued_samples     = LOAD(s_counters.queued_samples);
    out->recovery_samples   = LOAD(s_counters.recovery_samples);
    out->requested_rate_mhz = LOAD(s_counters.requested_rate_mhz);
    out->selected_rate      = LOAD(s_counters.selected_rate);
    out->acks_sent          = LOAD(s_counters.acks_sent);
#undef LOAD
}

esp_err_t idn_protocol_cancel_output(uint32_t timeout_ms)
{
    if (!s_server_running) {
        return ESP_OK;
    }

    xSemaphoreTake(s_cancel_done, 0);
    __atomic_store_n(&s_cancel_requested, true, __ATOMIC_RELEASE);

    if (xSemaphoreTake(s_cancel_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_cancel_result;
}

esp_err_t idn_protocol_start(void)
{
    ESP_LOGI(TAG, "Starting IDN protocol (IDN-Hello + IDN-RT)");

    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_ETH));
    ESP_LOGI(TAG, "IDN MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    s_ring = heap_caps_malloc(IDN_RING_CAPACITY * sizeof(rp_sample_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring)
        s_ring = heap_caps_malloc(IDN_RING_CAPACITY * sizeof(rp_sample_t),
                                  MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "Ring alloc failed (%u bytes)",
                 (unsigned)(IDN_RING_CAPACITY * sizeof(rp_sample_t)));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IDN ring: %u samples (%u bytes)",
             IDN_RING_CAPACITY,
             (unsigned)(IDN_RING_CAPACITY * sizeof(rp_sample_t)));

    s_feed_wake   = xSemaphoreCreateBinary();
    s_feed_done   = xSemaphoreCreateBinary();
    s_cancel_done = xSemaphoreCreateBinary();
    if (!s_feed_wake || !s_feed_done || !s_cancel_done) return ESP_ERR_NO_MEM;

    memset(s_decoders, 0, sizeof(s_decoders));
    idn_decoder_default_caps(&s_decoder_caps);
    idn_session_init(&s_session, &s_session_ops);

    BaseType_t ok = xTaskCreatePinnedToCore(
        server_task, "idn_server", STACK_SERVER, NULL, 5, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    return ESP_OK;
}
