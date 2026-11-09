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
 * One UDP address:port session at a time.
 * Link times out after 1 second of no packets (IDN-Hello spec section7.2).
 * On timeout or graceful close: feed stops, ring clears, channels reset.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#include "protocol_stream_stats.h"

_Static_assert(IDN_DECODER_CHANNELS == RP_SAMPLE_CHANNELS, "decoder and RP channel counts differ");

/* -- Tunables --------------------------------------------------------------- */
#define IDN_UDP_PORT            IDN_PROTOCOL_UDP_PORT
#define IDN_LINK_TIMEOUT_US     6000000LL       /* tolerate short sender/UI stalls */
#define IDN_RATE_HYSTERESIS     100u            /* pps */
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
#define IDNCMD_RT_CHANNEL       0x40
#define IDNCMD_RT_CHANNEL_ACK   0x41
#define IDNCMD_RT_CLOSE         0x44
#define IDNCMD_RT_CLOSE_ACK     0x45
#define IDNCMD_RT_ACK           0x47

/* -- IDN-Stream chunk types ------------------------------------------------- */
#define CHUNK_VOID              0x00
#define CHUNK_WAVE              0x01
#define CHUNK_FRAME_FULL        0x02
#define CHUNK_FRAME_FIRST       0x03
#define CHUNK_FRAGMENT_MIN      0xC0

/* -- IDN service types ------------------------------------------------------ */
#define IDNSVC_LASER            0x80

/* -- ACK result codes ------------------------------------------------------- */
#define IDNACK_OK               0x00
#define IDNACK_OCCUPIED         0xEC
#define IDNACK_INVALID          0xEE

/* -- Packed wire structures ------------------------------------------------- */
#pragma pack(push, 1)

/* IDN-Stream channel message header: 8 bytes */
typedef struct {
    uint16_t total_size;    /* big-endian */
    uint8_t  cnl;           /* CHANNELMSG(bit7,always1) | CONFIG_FLAG/CCLF(bit6) | channel_id(bits5-0) */
    uint8_t  chunk_type;
    uint32_t timestamp;     /* big-endian, uss */
} idn_stream_hdr_t;

/* Channel configuration header: 4 bytes */
typedef struct {
    uint8_t scwc;           /* service config word count (words x 4 = bytes) */
    uint8_t cfl;            /* bit0=Routing, bit1=Close */
    uint8_t service_id;
    uint8_t service_mode;
} idn_chan_cfg_hdr_t;

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

/* Acknowledge header: 12 bytes (IDNHDR_RT_ACKNOWLEDGE) */
typedef struct {
    uint8_t  struct_size;
    uint8_t  result_code;
    uint16_t input_event_flags;     /* big-endian */
    uint16_t pipeline_event_flags;  /* big-endian */
    uint8_t  status_flags;
    uint8_t  link_quality;
    uint32_t latency;               /* big-endian, uss */
} idn_ack_hdr_t;

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

/* -- Channel state ---------------------------------------------------------- */
typedef struct {
    bool                 open;
    uint8_t              service_mode;
    idn_decoder_layout_t layout;        /* invalid until a configuration compiles */
    idn_decoder_reason_t last_reject;   /* logged once per change */
} idn_channel_t;

/* -- SPSC ring buffer (decoded RP samples) --------------------------------- */
typedef struct {
    int16_t ch[RP_SAMPLE_CHANNELS];
} rp_sample_t;

static rp_sample_t       *s_ring      = NULL;
static volatile uint32_t  s_ring_head = 0;
static volatile uint32_t  s_ring_tail = 0;
static uint32_t           s_applied_rate = 0;     /* rate the RP runs */
static uint32_t           s_requested_rate = 0;   /* rate the sender's chunks imply */

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

static void apply_sample_rate(uint32_t rate)
{
    if (rate < 1000 || rate > 100000) {
        return;
    }

    /*
       Compare with what was asked, not what the RP applied: a sender above
       the scanner limit asks for the same rate in every chunk, and must not
       cost a UART command per chunk.
    */
    uint32_t diff = s_requested_rate > rate ? s_requested_rate - rate : rate - s_requested_rate;
    if (s_requested_rate != 0 && diff < IDN_RATE_HYSTERESIS) {
        return;
    }

    uint32_t applied = rate;
    if (rp_manager_set_rate_applied(rate, &applied) == ESP_OK) {
        s_requested_rate = rate;
        s_applied_rate = applied;
        if (applied != rate) {
            ESP_LOGW(TAG, "Sender rate %lu pps above scanner limit; running %lu, excess will be dropped",
                     (unsigned long)rate, (unsigned long)applied);
        }
    }
}

/* -- Module globals --------------------------------------------------------- */

static uint8_t s_mac[6];
static int     s_sock = -1;

/* Session */
static struct sockaddr_in s_client_addr;
static bool               s_client_active = false;
static int64_t            s_link_last_us  = 0;

/* Channels */
static idn_channel_t s_channels[IDN_MAX_CHANNELS];

/* Feed */
static volatile bool s_feed_running = false;
static volatile bool s_output_armed = false;
static TaskHandle_t  s_feed_task_h  = NULL;

/* Feed buffers (static = no stack pressure) */
static int16_t     s_feed_packet[RP_STREAM_SAMPLES_PER_PACKET][RP_SAMPLE_CHANNELS];
static rp_sample_t s_feed_stage[RP_STREAM_SAMPLES_PER_PACKET];

/* -- Decoder ---------------------------------------------------------------- */
static idn_decoder_caps_t s_decoder_caps;

static void log_decoder_reject(uint8_t ch_id, idn_channel_t *ch, const char *what,
                               idn_decoder_result_t r)
{
    if (ch->last_reject == r.reason) return;
    ch->last_reject = r.reason;
    ESP_LOGW(TAG, "Channel %u %s rejected: %s (%s), tag 0x%04X at word %u",
             ch_id, what, idn_decoder_status_name(r.status), idn_decoder_reason_name(r.reason),
             r.tag, r.tag_index);
}

/* -- Feed task -------------------------------------------------------------- */
static void feed_task(void *arg)
{
    (void)arg;
    s_feed_running = true;

    /* Initial "last known" point: blanked, at origin */
    rp_sample_t last;
    memset(&last, 0, sizeof(last));
    last.ch[2] = last.ch[3] = last.ch[4] = last.ch[5] = -32768;

    bool output_started = false;
    uint32_t queued_before_start = 0;
    uint32_t last_avail = 0;
    int64_t last_growth_us = esp_timer_get_time();

    ESP_LOGI(TAG, "IDN feed task started");

    while (s_feed_running) {
        if (!s_output_armed) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

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
                if (avail == 0 && idle && !output_started && queued_before_start > 0) {
                    rp_manager_output_start();
                    output_started = true;
                    ESP_LOGI(TAG, "IDN RP output started");
                }
                vTaskDelay(1);
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

            /* Start once a short cushion is queued, or when a short stream is flushed. */
            if (!output_started &&
                (queued_before_start >= IDN_FEED_PREBUFFER || got < (uint32_t)RP_STREAM_SAMPLES_PER_PACKET)) {
                rp_manager_output_start();
                output_started = true;
                ESP_LOGI(TAG, "IDN RP output started");
            }
            protocol_stream_stats_add_samples(PROTOCOL_STREAM_IDN, got);
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
    ESP_LOGI(TAG, "IDN feed task exiting");
    s_feed_task_h = NULL;
    vTaskDelete(NULL);
}

static esp_err_t feed_start(void)
{
    /* Wait briefly in case a prior task is still winding down after a
     * non-blocking feed_stop().  Normal exit takes < 10 ms; the 300 ms
     * budget is a safety net for a task blocked inside send_samples_packet. */
    for (int i = 0; i < 30 && s_feed_task_h != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_feed_task_h != NULL) {
        /* Task is still alive after 300 ms - rearm it rather than spawning
         * a second one.  s_output_armed will be set by process_wave_chunk(). */
        s_feed_running = true;
        return ESP_OK;
    }

    s_feed_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        feed_task, "idn_feed", STACK_FEED, NULL, IDN_FEED_TASK_PRIORITY, &s_feed_task_h, IDN_FEED_TASK_CORE);
    if (ok != pdPASS) {
        s_feed_running = false;
        s_feed_task_h  = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void feed_stop(void)
{
    s_feed_running = false;
    s_output_armed = false;
    /* Non-blocking: the feed task checks s_feed_running on every loop
     * iteration and exits on its own.  feed_start() will wait if needed. */
}

/* -- Session management ----------------------------------------------------- */
static void session_close(void)
{
    if (!s_client_active) return;
    ESP_LOGI(TAG, "IDN session closed");
    feed_stop();
    ring_clear();
    memset(s_channels, 0, sizeof(s_channels));
    s_client_active = false;
    s_applied_rate = 0;
    s_requested_rate = 0;
}

static void session_check_timeout(void)
{
    if (!s_client_active) return;
    if (esp_timer_get_time() - s_link_last_us > IDN_LINK_TIMEOUT_US) {
        ESP_LOGW(TAG, "IDN link timeout");
        session_close();
    }
}

/*
 * Called on every valid RT packet from a client.
 * Opens a new session if none active; touches the timeout on existing one.
 * Returns false if the packet is from a different (blocked) client.
 */
static bool session_touch(const struct sockaddr_in *from)
{
    if (!s_client_active) {
        s_client_addr   = *from;
        s_client_active = true;
        ESP_LOGI(TAG, "IDN new session from %s:%d",
                 inet_ntoa(from->sin_addr), ntohs(from->sin_port));
        feed_start();
        apply_sample_rate(30000);   /* safe default; overridden by wave chunk duration */
    } else {
        /* Match by IP only - source port can change on sender reconnect */
        if (from->sin_addr.s_addr != s_client_addr.sin_addr.s_addr) {
            return false;   /* different host, occupied */
        }
        if (from->sin_port != s_client_addr.sin_port) {
            ESP_LOGI(TAG, "IDN client port changed %d->%d",
                     ntohs(s_client_addr.sin_port), ntohs(from->sin_port));
            s_client_addr.sin_port = from->sin_port;
        }
    }
    s_link_last_us = esp_timer_get_time();
    return true;
}

/* -- Send helpers ----------------------------------------------------------- */

/*
 * Send an IDN-Hello packet.  payload may be NULL if payload_len == 0.
 * Uses a stack buffer for small payloads (management responses).
 * For large payloads (ping echo) we allocate on heap.
 */
static void send_pkt(const struct sockaddr_in *dest,
                     uint8_t cmd, uint8_t flags, uint16_t seq,
                     const void *payload, size_t payload_len)
{
    size_t total = 4 + payload_len;
    uint8_t *buf;
    uint8_t  sbuf[128];
    bool     heap = (total > sizeof(sbuf));

    if (heap) {
        buf = malloc(total);
        if (!buf) return;
    } else {
        buf = sbuf;
    }

    buf[0] = cmd;
    buf[1] = flags;
    wr16(&buf[2], seq);
    if (payload && payload_len)
        memcpy(&buf[4], payload, payload_len);

    sendto(s_sock, buf, total, 0,
           (const struct sockaddr *)dest, sizeof(*dest));

    if (heap) free(buf);
}

static void send_ack(const struct sockaddr_in *dest,
                     uint8_t flags, uint16_t seq, uint8_t result_code)
{
    idn_ack_hdr_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.struct_size = sizeof(ack);
    ack.result_code = result_code;
    send_pkt(dest, IDNCMD_RT_ACK, flags, seq, &ack, sizeof(ack));
}

/* -- Discovery handlers ----------------------------------------------------- */
static void handle_scan_req(const struct sockaddr_in *from,
                             uint8_t flags, uint16_t seq)
{
    idn_scan_resp_t r;
    memset(&r, 0, sizeof(r));
    r.struct_size       = sizeof(r);
    r.protocol_version  = 0x01;     /* IDN-Hello v1: major=0, minor=1 */
    r.status            = 0x01;     /* bit0=RT supported */

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

/* -- Channel configuration -------------------------------------------------- */
static void process_channel_config(uint8_t ch_id,
                                    const idn_chan_cfg_hdr_t *cfg,
                                    const uint8_t *svc_data,
                                    uint16_t svc_bytes)
{
    if (ch_id >= IDN_MAX_CHANNELS) return;
    idn_channel_t *ch = &s_channels[ch_id];

    if (cfg->cfl & 0x01) {   /* Routing bit: open/configure */
        if (!ch->open)
            ESP_LOGI(TAG, "Channel %u opened, mode=0x%02X",
                     ch_id, cfg->service_mode);
        ch->open         = true;
        ch->service_mode = cfg->service_mode;

        /* Every routing configuration is compiled; a rejected one leaves the channel without a decoder. */
        idn_decoder_result_t r = idn_decoder_compile(svc_data, svc_bytes, &s_decoder_caps, &ch->layout);
        if (r.status == IDN_DECODER_OK) {
            if (ch->last_reject != IDN_DECODER_REASON_NONE)
                ESP_LOGI(TAG, "Channel %u configuration accepted", ch_id);
            ch->last_reject = IDN_DECODER_REASON_NONE;
            ESP_LOGD(TAG, "Tag dict: %u bytes/sample", ch->layout.stride);
        } else {
            log_decoder_reject(ch_id, ch, "configuration", r);
        }
    }
}

/* -- Wave chunk processing -------------------------------------------------- */
static void process_wave_chunk(uint8_t ch_id,
                                const uint8_t *data, uint16_t data_len)
{
    if (ch_id >= IDN_MAX_CHANNELS) return;
    idn_channel_t *ch = &s_channels[ch_id];

    /* No auto-open and no default layout: data needs a routed channel whose configuration compiled. */
    if (!ch->open || !ch->layout.valid) {
        ESP_LOGD(TAG, "Wave chunk on channel %u without accepted configuration dropped", ch_id);
        return;
    }

    /* Whole chunk is validated (header, duration, exact stride, >= 20 samples) before decoding. */
    uint32_t nsamples = 0, duration_us = 0;
    idn_decoder_result_t r = idn_decoder_check_wave_chunk(&ch->layout, data, data_len,
                                                          &nsamples, &duration_us);
    if (r.status != IDN_DECODER_OK) {
        log_decoder_reject(ch_id, ch, "wave chunk", r);
        return;
    }

    const uint8_t *samples = data + IDN_DECODER_WAVE_HEADER_BYTES;
    uint8_t        stride  = ch->layout.stride;

    /* Derive sample rate from the chunk duration field (24-bit BE uss). */
    uint32_t rate = (uint32_t)((uint64_t)nsamples * 1000000ULL / duration_us);
    apply_sample_rate(rate);

    /* Queue the whole chunk or none of it; this task is the only producer. */
    if (nsamples > ring_free()) {
        protocol_stream_stats_add_dropped(PROTOCOL_STREAM_IDN, nsamples);
        return;
    }

    rp_sample_t burst[64];
    uint32_t    written  = 0;

    for (uint32_t s = 0; s < nsamples; ) {
        uint32_t n = nsamples - s;
        if (n > 64) n = 64;

        for (uint32_t j = 0; j < n; j++)
            idn_decoder_decode(&ch->layout, samples + (size_t)(s + j) * stride, 1, &burst[j].ch);

        uint32_t w = ring_write(burst, n);
        written += w;
        if (w < n) {
            /* Ring full: the rest of this chunk is lost. Count it. */
            protocol_stream_stats_add_dropped(PROTOCOL_STREAM_IDN, nsamples - s - w);
            break;
        }
        s += n;
    }

    if (written > 0 && !s_output_armed)
        s_output_armed = true;
}

/* -- IDN-RT packet handler -------------------------------------------------- */
static void handle_rt_packet(const struct sockaddr_in *from,
                              uint8_t cmd, uint8_t flags, uint16_t seq,
                              const uint8_t *payload, uint16_t payload_len)
{
    bool need_ack = (cmd == IDNCMD_RT_CHANNEL_ACK || cmd == IDNCMD_RT_CLOSE_ACK);
    bool do_close = (cmd == IDNCMD_RT_CLOSE       || cmd == IDNCMD_RT_CLOSE_ACK);

    /* Check if occupied by a different client (match IP only; port can change) */
    if (s_client_active &&
        from->sin_addr.s_addr != s_client_addr.sin_addr.s_addr) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_OCCUPIED);
        return;
    }

    /* A close with no session open (a repeated close, say) must not open one. */
    if (!s_client_active && do_close && payload_len == 0) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_OK);
        return;
    }

    if (!session_touch(from)) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_OCCUPIED);
        return;
    }

    /* Empty payload = keepalive */
    if (payload_len == 0) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_OK);
        if (do_close) session_close();
        return;
    }

    /* Need at least the 8-byte IDN-Stream channel message header */
    if (payload_len < (uint16_t)sizeof(idn_stream_hdr_t)) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_INVALID);
        return;
    }

    const idn_stream_hdr_t *smsg = (const idn_stream_hdr_t *)payload;
    uint16_t msg_size   = rd16((const uint8_t *)&smsg->total_size);
    uint8_t  cnl        = smsg->cnl;
    uint8_t  chunk_type = smsg->chunk_type;
    bool     cclf       = (cnl & 0x40) != 0;   /* bit6=CONFIG_LSTFRG; bit7 is always 1 (CHANNELMSG) */
    uint8_t  ch_id      = cnl & 0x3F;

    /* Sanity check */
    if (msg_size < (uint16_t)sizeof(idn_stream_hdr_t) || msg_size > payload_len) {
        if (need_ack) send_ack(from, flags, seq, IDNACK_INVALID);
        return;
    }

    const uint8_t *cursor = payload + sizeof(idn_stream_hdr_t);
    uint16_t cursor_len   = msg_size - (uint16_t)sizeof(idn_stream_hdr_t);

    /* -- Channel configuration header ------------------------------------ */
    bool close_channel_after = false;

    if (cclf && chunk_type < CHUNK_FRAGMENT_MIN) {
        if (cursor_len < (uint16_t)sizeof(idn_chan_cfg_hdr_t)) {
            if (need_ack) send_ack(from, flags, seq, IDNACK_INVALID);
            return;
        }
        const idn_chan_cfg_hdr_t *cfg = (const idn_chan_cfg_hdr_t *)cursor;
        cursor     += sizeof(idn_chan_cfg_hdr_t);
        cursor_len -= (uint16_t)sizeof(idn_chan_cfg_hdr_t);

        uint16_t svc_bytes = (uint16_t)cfg->scwc * 4;
        if (svc_bytes > cursor_len) {
            /* Never shorten the declared configuration to fit; nothing on this channel decodes until a whole one arrives. */
            if (ch_id < IDN_MAX_CHANNELS)
                s_channels[ch_id].layout.valid = false;
            if (need_ack) send_ack(from, flags, seq, IDNACK_INVALID);
            return;
        }

        process_channel_config(ch_id, cfg, cursor, svc_bytes);

        cursor     += svc_bytes;
        cursor_len -= svc_bytes;

        close_channel_after = (cfg->cfl & 0x02) != 0;
    }

    /* -- Data chunk ------------------------------------------------------- */
    if (chunk_type == CHUNK_VOID) {
        /* no-op, used for keepalive or config-only messages */
    } else if (chunk_type == CHUNK_WAVE) {
        if (cursor_len > 0)
            process_wave_chunk(ch_id, cursor, cursor_len);
    } else if (chunk_type < CHUNK_FRAGMENT_MIN) {
        /* Frame samples (discrete mode) - not implemented, silently skip */
        ESP_LOGD(TAG, "Unhandled chunk type 0x%02X on channel %u",
                 chunk_type, ch_id);
    } else {
        /* Fragment continuation (0xC0+) - not implemented */
    }

    /* -- Channel close ---------------------------------------------------- */
    if (close_channel_after && ch_id < IDN_MAX_CHANNELS) {
        s_channels[ch_id].open = false;
        ESP_LOGI(TAG, "Channel %u closed", ch_id);

        bool any_open = false;
        for (int i = 0; i < IDN_MAX_CHANNELS; i++)
            if (s_channels[i].open) { any_open = true; break; }
        if (!any_open) session_close();
    }

    if (need_ack) send_ack(from, flags, seq, IDNACK_OK);
    if (do_close) session_close();
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
     * rxbuf: 1500 bytes covers a full Ethernet MTU payload.
     * Wave sample packets can be up to ~1480 bytes (payload after IDN headers).
     * Declared static to keep it off the task stack.
     */
    static uint8_t rxbuf[1500];

    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(s_sock, rxbuf, sizeof(rxbuf), 0,
                         (struct sockaddr *)&from, &from_len);

        /* Check link timeout on every loop iteration, even on recv timeout */
        session_check_timeout();

        if (n < 4) continue;    /* too short or EAGAIN from SO_RCVTIMEO */

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

            case IDNCMD_SCAN_REQ:
                handle_scan_req(&from, flags, seq);
                break;

            case IDNCMD_SVCMAP_REQ:
                handle_svcmap_req(&from, flags, seq);
                break;

            case IDNCMD_RT_CHANNEL:
            case IDNCMD_RT_CHANNEL_ACK:
            case IDNCMD_RT_CLOSE:
            case IDNCMD_RT_CLOSE_ACK:
                handle_rt_packet(&from, cmd, flags, seq, payload, payload_len);
                break;

            default:
                /* Silently discard unknown commands */
                break;
        }
    }
}

/* -- Public init ------------------------------------------------------------ */
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

    memset(s_channels, 0, sizeof(s_channels));
    idn_decoder_default_caps(&s_decoder_caps);

    BaseType_t ok = xTaskCreatePinnedToCore(
        server_task, "idn_server", STACK_SERVER, NULL, 5, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    return ESP_OK;
}
