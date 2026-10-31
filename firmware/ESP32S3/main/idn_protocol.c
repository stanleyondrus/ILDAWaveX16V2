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
 * Tags are 16-bit BE words structured as nibbles [CAT][SUB][ID][PRM].
 * Octet-descriptor tags (cat 4, 5) consume one sample byte each, in order.
 * Key tags we handle:
 *   0x4200  X coordinate (8-bit signed, left-aligned)
 *   0x4010  Precision - adds one low byte to the preceding coordinate tag
 *   0x4210  Y coordinate
 *   0x5NNN  Color (10-bit wavelength NNN in 4nm steps; mapped R/G/B by range)
 *   0x5C10  Intensity (blanking / legacy ISP signal)
 *   0x41xx  Hint (shutter / scale; byte consumed, otherwise ignored)
 *   0x000x  Void (skip x suffix words, no sample byte)
 *   0x1xxx  Modifier (no sample byte)
 *
 * ISP-DB25 default layout (used when no tag dict is supplied):
 *   X_hi X_lo Y_hi Y_lo R G B I  ->  8 bytes/sample, 16-bit X/Y, 8-bit colors
 *
 * -- Sample conversion -> RP int16 ---------------------------------------------
 * RP channels: ch[0]=X, ch[1]=Y, ch[2]=R, ch[3]=G, ch[4]=B, ch[5]=I, 6-7=0
 * X/Y: 16-bit signed left-aligned (hi<<8|lo) -> already int16
 *       8-bit only: (int8_t)hi << 8  scales -128..127 -> -32768..32512
 * Colors: uint8 -> int16 via  (int16_t)(v * 256 - 32768)
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
#include "protocol_stream_stats.h"

/* -- Tunables --------------------------------------------------------------- */
#define IDN_UDP_PORT            IDN_PROTOCOL_UDP_PORT
#define IDN_LINK_TIMEOUT_US     6000000LL       /* tolerate short sender/UI stalls */
#define IDN_RATE_HYSTERESIS     100u            /* pps */
#define IDN_RING_CAPACITY       8192u           /* must be power of 2 */
#define IDN_FEED_FLUSH_US       100000LL        /* send a partial packet only after the stream idles this long */
#define IDN_FEED_PREBUFFER      3072u           /* samples queued on the RP before START (~100 ms at 30 kpps) */
#define IDN_MAX_CHANNELS        64
#define IDN_MAX_SAMPLE_OCTETS   32
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

/* -- Sample layout (built from tag dictionary) ------------------------------ */
typedef enum {
    ROLE_IGNORE = 0,
    ROLE_X_HI,      /* high octet of X (signed, left-aligned) */
    ROLE_X_LO,      /* precision (low) octet of X */
    ROLE_Y_HI,
    ROLE_Y_LO,
    ROLE_R,
    ROLE_G,
    ROLE_B,
    ROLE_I,         /* intensity / blanking */
} octet_role_t;

typedef struct {
    bool         valid;
    uint8_t      bps;                          /* bytes per sample */
    octet_role_t role[IDN_MAX_SAMPLE_OCTETS];
} sample_layout_t;

/* -- Channel state ---------------------------------------------------------- */
typedef struct {
    bool            open;
    uint8_t         service_mode;
    sample_layout_t layout;
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

/* -- Tag dictionary parser -------------------------------------------------- */
static void install_default_layout(sample_layout_t *lay)
{
    memset(lay, 0, sizeof(*lay));
    lay->valid   = true;
    lay->bps     = 8;
    lay->role[0] = ROLE_X_HI;
    lay->role[1] = ROLE_X_LO;
    lay->role[2] = ROLE_Y_HI;
    lay->role[3] = ROLE_Y_LO;
    lay->role[4] = ROLE_R;
    lay->role[5] = ROLE_G;
    lay->role[6] = ROLE_B;
    lay->role[7] = ROLE_I;
}

/*
 * Parse a tag-dictionary byte array (SCWC x 4 bytes of 16-bit BE tag words)
 * into a sample_layout_t.  Returns false and installs default on failure.
 */
static bool parse_tag_dict(const uint8_t *data, uint16_t len, sample_layout_t *lay)
{
    memset(lay, 0, sizeof(*lay));

    uint16_t     pos              = 0;
    uint8_t      oct              = 0;       /* current sample octet index */
    octet_role_t prev_coord_role  = ROLE_IGNORE;
    bool         has_r = false, has_g = false, has_b = false;

    while (pos + 1 < len && oct < IDN_MAX_SAMPLE_OCTETS) {
        uint16_t tag = rd16(data + pos);
        pos += 2;

        uint8_t cat = (tag >> 12) & 0xF;
        uint8_t sub = (tag >>  8) & 0xF;
        uint8_t id  = (tag >>  4) & 0xF;
        uint8_t prm = (tag >>  0) & 0xF;

        /* Category 0: void / struct modifier */
        if (cat == 0) {
            /* Void tag: skip prm suffix words, no sample octet */
            pos += (uint16_t)(prm * 2);
            prev_coord_role = ROLE_IGNORE;
            continue;
        }

        /* Category 1: modifier with parameter (Break, Space) */
        if (cat == 1) {
            prev_coord_role = ROLE_IGNORE;
            continue;
        }

        /* Category 4 sub 0: NOP or Precision */
        if (cat == 4 && sub == 0) {
            if (id == 0 && prm == 0) {
                /* NOP: consumes one sample octet, ignored */
                lay->role[oct++] = ROLE_IGNORE;
                prev_coord_role  = ROLE_IGNORE;
            } else if (id == 1 && prm == 0) {
                /* Precision: low byte for previous coordinate */
                octet_role_t lo = ROLE_IGNORE;
                if      (prev_coord_role == ROLE_X_HI) lo = ROLE_X_LO;
                else if (prev_coord_role == ROLE_Y_HI) lo = ROLE_Y_LO;
                lay->role[oct++] = lo;
                /* don't reset prev_coord_role - multiple precision tags allowed */
            }
            continue;
        }

        /* Category 4 sub 1: Hint - consumes one sample octet */
        if (cat == 4 && sub == 1) {
            lay->role[oct++] = ROLE_IGNORE;   /* shutter/scale byte, we ignore */
            prev_coord_role  = ROLE_IGNORE;
            continue;
        }

        /* Category 4 sub 2: X / Y / Z coordinate */
        if (cat == 4 && sub == 2) {
            octet_role_t role = ROLE_IGNORE;
            if      (id == 0 && prm == 0) { role = ROLE_X_HI; prev_coord_role = ROLE_X_HI; }
            else if (id == 1 && prm == 0) { role = ROLE_Y_HI; prev_coord_role = ROLE_Y_HI; }
            else                          { prev_coord_role = ROLE_IGNORE; }
            lay->role[oct++] = role;
            continue;
        }

        /* Category 5 sub 0-3: color by wavelength.
         * The 10-bit field encodes wavelength in 4 nm steps, so
         *   wl_nm = wl * 4   (e.g. wl=160 -> 640 nm red, wl=133 -> 532 nm green).
         * Thresholds below are therefore in nm/4 units, not raw nm. */
        if (cat == 5 && sub <= 3) {
            uint16_t wl = (uint16_t)(((sub & 3u) << 8) | (id << 4) | prm);
            octet_role_t role;
            if      (wl >= 145 && wl <= 195) { role = ROLE_R; has_r = true; }
            else if (wl >= 122 && wl <  145) { role = ROLE_G; has_g = true; }
            else if (wl >=  95 && wl <  122) { role = ROLE_B; has_b = true; }
            else if (!has_r)                 { role = ROLE_R; has_r = true; }
            else if (!has_g)                 { role = ROLE_G; has_g = true; }
            else if (!has_b)                 { role = ROLE_B; has_b = true; }
            else                               role = ROLE_IGNORE;
            lay->role[oct++] = role;
            prev_coord_role  = ROLE_IGNORE;
            continue;
        }

        /* Category 5 sub 12 (0xC): Intensity (0x5C10), Beam-Brush, Wavelen Prefix */
        if (cat == 5 && sub == 0xC) {
            octet_role_t role = ROLE_IGNORE;
            if (id == 1 && prm == 0) role = ROLE_I;   /* 0x5C10 = Intensity */
            lay->role[oct++] = role;
            prev_coord_role  = ROLE_IGNORE;
            continue;
        }

        /* All other categories: unknown, no sample octet consumed */
        prev_coord_role = ROLE_IGNORE;
    }

    lay->bps = oct;

    /* Validate: need at least X, Y, and one color */
    bool has_x = false, has_y = false, has_color = false;
    for (uint8_t j = 0; j < oct; j++) {
        switch (lay->role[j]) {
            case ROLE_X_HI:  has_x     = true; break;
            case ROLE_Y_HI:  has_y     = true; break;
            case ROLE_R: case ROLE_G:
            case ROLE_B: case ROLE_I:
                has_color = true; break;
            default: break;
        }
    }

    if (!has_x || !has_y || !has_color) {
        ESP_LOGW(TAG, "Tag dict unusable (x=%d y=%d color=%d bps=%d) -> default",
                 has_x, has_y, has_color, oct);
        install_default_layout(lay);
        return false;
    }

    lay->valid = true;
    ESP_LOGD(TAG, "Tag dict: %u bytes/sample", oct);
    return true;
}

/* -- Sample decode ---------------------------------------------------------- */
static inline void decode_sample(const uint8_t *oct, const sample_layout_t *lay,
                                  rp_sample_t *out)
{
    int8_t  x_hi = 0;  uint8_t x_lo = 0;  bool have_xlo = false;
    int8_t  y_hi = 0;  uint8_t y_lo = 0;  bool have_ylo = false;
    uint8_t r = 0, g = 0, b = 0, intens = 0;

    for (uint8_t j = 0; j < lay->bps; j++) {
        uint8_t v = oct[j];
        switch (lay->role[j]) {
            case ROLE_X_HI:  x_hi = (int8_t)v;   break;
            case ROLE_X_LO:  x_lo = v; have_xlo = true; break;
            case ROLE_Y_HI:  y_hi = (int8_t)v;   break;
            case ROLE_Y_LO:  y_lo = v; have_ylo = true; break;
            case ROLE_R:     r      = v; break;
            case ROLE_G:     g      = v; break;
            case ROLE_B:     b      = v; break;
            case ROLE_I:     intens = v; break;
            default: break;
        }
    }

    /* X/Y: 16-bit if precision present, else 8-bit scaled */
    int16_t x = have_xlo ? (int16_t)(((int16_t)x_hi << 8) | x_lo)
                          : (int16_t)(x_hi << 8);
    int16_t y = have_ylo ? (int16_t)(((int16_t)y_hi << 8) | y_lo)
                          : (int16_t)(y_hi << 8);

    /* Derive intensity from colour when sender omits ROLE_I */
    if (intens == 0 && (r | g | b)) {
        intens = (r >= g && r >= b) ? r : (g >= b ? g : b);
    }

    /* Colors: uint8 -> int16  (0 -> -32768, 255 -> +32512) */
    out->ch[0] = x;
    out->ch[1] = y;
    out->ch[2] = (int16_t)((int32_t)r      * 256 - 32768);
    out->ch[3] = (int16_t)((int32_t)g      * 256 - 32768);
    out->ch[4] = (int16_t)((int32_t)b      * 256 - 32768);
    out->ch[5] = (int16_t)((int32_t)intens * 256 - 32768);
    out->ch[6] = 0;
    out->ch[7] = 0;
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

        if (cfg->scwc > 0 && svc_bytes >= 2) {
            parse_tag_dict(svc_data, svc_bytes, &ch->layout);
        } else if (!ch->layout.valid) {
            install_default_layout(&ch->layout);
        }
    }
}

/* -- Wave chunk processing -------------------------------------------------- */
static void process_wave_chunk(uint8_t ch_id,
                                const uint8_t *data, uint16_t data_len)
{
    if (ch_id >= IDN_MAX_CHANNELS) return;
    idn_channel_t *ch = &s_channels[ch_id];

    /* Auto-open with default layout if client sends data without routing */
    if (!ch->open) {
        ch->open = true;
        if (!ch->layout.valid)
            install_default_layout(&ch->layout);
        ESP_LOGD(TAG, "Channel %u auto-opened", ch_id);
    }

    if (data_len < 4) return;   /* need chunk header */

    /* Wave chunk header: flags(1) + duration uss (3 bytes BE) */
    const uint8_t *samples = data + 4;
    uint16_t       slen    = data_len - 4;
    uint8_t        bps     = ch->layout.bps;

    if (bps == 0 || slen < bps) return;

    uint32_t nsamples = slen / bps;

    /* Derive sample rate from the chunk duration field (24-bit BE uss).
     * Only update when both fields are non-zero to avoid divide-by-zero
     * and to ignore keepalive/config-only chunks. */
    uint32_t duration_us = ((uint32_t)data[1] << 16) |
                           ((uint32_t)data[2] <<  8) |
                            (uint32_t)data[3];
    if (duration_us > 0 && nsamples > 0) {
        uint32_t rate = (uint32_t)((uint64_t)nsamples * 1000000ULL / duration_us);
        apply_sample_rate(rate);
    }

    rp_sample_t burst[64];
    uint32_t    written  = 0;

    for (uint32_t s = 0; s < nsamples; ) {
        uint32_t n = nsamples - s;
        if (n > 64) n = 64;

        for (uint32_t j = 0; j < n; j++)
            decode_sample(samples + (s + j) * bps, &ch->layout, &burst[j]);

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
        if (svc_bytes > cursor_len) svc_bytes = cursor_len;

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

    BaseType_t ok = xTaskCreatePinnedToCore(
        server_task, "idn_server", STACK_SERVER, NULL, 5, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    return ESP_OK;
}
