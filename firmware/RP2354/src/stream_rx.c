#include <string.h>

#include "pico/stdlib.h"

#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "spi_payload_rx.pio.h"

#include "ring_buffer.h"
#include "rp_uart.h"
#include "stream_rx.h"

/* ESP -> RP SPI payload */
#define RP_SPI_RX_GPIO   12
#define RP_SPI_CSN_GPIO  13
#define RP_SPI_SCK_GPIO  14

/* READY / flow control */
#define RP_READY_GPIO 15

/* Stream packet */
#define STREAM_MAGIC0 0xA5
#define STREAM_MAGIC1 0x53

#define STREAM_HEADER_BYTES 12
#define STREAM_PAYLOAD_BYTES (STREAM_SAMPLES_PER_PACKET * RP_SAMPLE_CHANNELS * 2)
#define STREAM_PACKET_BYTES (STREAM_HEADER_BYTES + STREAM_PAYLOAD_BYTES)

static volatile uint32_t s_payload_errors = 0;
static volatile uint32_t s_received_blocks = 0;
static volatile uint32_t s_received_samples = 0;

static volatile uint32_t s_stream_packets = 0;
static volatile uint32_t s_stream_bad_packets = 0;
static volatile uint32_t s_stream_dropped_packets = 0;
static volatile uint32_t s_stream_rx_timeouts = 0;
static volatile bool s_flow_paused = false;
static volatile uint32_t s_stale_packets = 0;
static uint32_t s_packet_epoch = 0;
static bool s_rx_armed = false;

/*
   Diagnostics for packets lost between the ESP32 and the ring: gaps in the
   ESP32's sequence numbers within one stream (epoch), and receives that
   were cancelled after part of a packet had arrived.
*/
static volatile uint32_t s_seq_gaps = 0;
static volatile uint32_t s_seq_missing = 0;
static volatile uint32_t s_rx_partial = 0;
static bool s_seq_valid = false;
static uint16_t s_seq_expected = 0;
static uint32_t s_seq_epoch = 0;

static uint32_t s_packet_words[STREAM_PACKET_BYTES];
static uint8_t s_packet_bytes[STREAM_PACKET_BYTES];

static PIO s_payload_pio = pio0;
static uint s_payload_sm = 0;
static uint s_payload_offset = 0;
static int s_payload_dma_chan = -1;

static uint8_t reverse8(uint8_t x)
{
    x = (uint8_t)(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    return x;
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t checksum32_payload(const uint8_t *data, uint32_t len)
{
    uint32_t c = 0;

    for (uint32_t i = 0; i < len; i++) {
        c += data[i];
    }

    return c;
}

static void ready_gpio_init(void)
{
    gpio_init(RP_READY_GPIO);
    gpio_set_dir(RP_READY_GPIO, GPIO_OUT);
    gpio_put(RP_READY_GPIO, 0);
}

void stream_rx_init(void)
{
    ready_gpio_init();

    s_payload_offset = pio_add_program(s_payload_pio, &spi_payload_rx_program);

    pio_sm_config c = spi_payload_rx_program_get_default_config(s_payload_offset);

    sm_config_set_in_pins(&c, RP_SPI_RX_GPIO);
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    pio_sm_init(s_payload_pio, s_payload_sm, s_payload_offset, &c);

    gpio_set_function(RP_SPI_RX_GPIO, GPIO_FUNC_PIO0);
    gpio_set_function(RP_SPI_CSN_GPIO, GPIO_FUNC_PIO0);
    gpio_set_function(RP_SPI_SCK_GPIO, GPIO_FUNC_PIO0);
    gpio_pull_up(RP_SPI_CSN_GPIO);

    pio_sm_set_enabled(s_payload_pio, s_payload_sm, false);

    s_payload_dma_chan = dma_claim_unused_channel(true);
}

static bool rx_nothing_arrived(void)
{
    return dma_hw->ch[s_payload_dma_chan].transfer_count == STREAM_PACKET_BYTES;
}

static void rx_arm(void)
{
    gpio_put(RP_READY_GPIO, 0);

    pio_sm_set_enabled(s_payload_pio, s_payload_sm, false);
    pio_sm_clear_fifos(s_payload_pio, s_payload_sm);
    pio_sm_restart(s_payload_pio, s_payload_sm);
    pio_sm_exec(s_payload_pio, s_payload_sm, pio_encode_jmp(s_payload_offset));

    dma_channel_config cfg = dma_channel_get_default_config(s_payload_dma_chan);

    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(s_payload_pio, s_payload_sm, false));

    dma_channel_configure(
        s_payload_dma_chan,
        &cfg,
        s_packet_words,
        &s_payload_pio->rxf[s_payload_sm],
        STREAM_PACKET_BYTES,
        false
    );

    dma_channel_start(s_payload_dma_chan);
    pio_sm_set_enabled(s_payload_pio, s_payload_sm, true);

    /* Remember which stream this packet belongs to (see stream_rx_process_packet). */
    s_packet_epoch = ring_get_epoch();
    s_rx_armed = true;

    sleep_us(5);
    gpio_put(RP_READY_GPIO, 1);
}

void stream_rx_cancel(void)
{
    if (!s_rx_armed) {
        return;
    }

    gpio_put(RP_READY_GPIO, 0);
    if (!rx_nothing_arrived()) {
        s_rx_partial++;
    }
    dma_channel_abort(s_payload_dma_chan);
    pio_sm_set_enabled(s_payload_pio, s_payload_sm, false);
    s_rx_armed = false;
}

/*
   Once READY is raised it stays raised, with the DMA armed, until a whole
   packet is in: a timeout only returns to the main loop. Dropping READY on
   a timeout lost any packet the ESP32 had started sending in the last
   millisecond of the wait (4108 bytes at 32 MHz), and the stream jumped a
   full packet ahead, lit. The ring only drains while a receive is armed, so
   the room checked at arming time is still there when the packet lands.
*/
bool stream_rx_receive_packet(uint32_t timeout_ms)
{
    if (!s_rx_armed) {
        uint32_t fill = ring_get_fill();

        if (s_flow_paused) {
            if (fill <= ring_get_low_water()) {
                s_flow_paused = false;
            } else {
                gpio_put(RP_READY_GPIO, 0);
                return false;
            }
        }

        if (fill >= ring_get_high_water()) {
            s_flow_paused = true;
            gpio_put(RP_READY_GPIO, 0);
            return false;
        }

        rx_arm();
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (dma_channel_is_busy(s_payload_dma_chan)) {
        /*
           A STOP or CLEAR handled while nothing has arrived yet makes the
           coming packet part of the new stream; one that lands mid-packet
           leaves it stale.
        */
        if (rx_nothing_arrived()) {
            s_packet_epoch = ring_get_epoch();
        }

        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            s_stream_rx_timeouts++;
            return false;
        }

        rp_uart_poll();
    }

    gpio_put(RP_READY_GPIO, 0);
    pio_sm_set_enabled(s_payload_pio, s_payload_sm, false);
    s_rx_armed = false;

    for (uint32_t i = 0; i < STREAM_PACKET_BYTES; i++) {
        s_packet_bytes[i] = reverse8((uint8_t)((s_packet_words[i] >> 24) & 0xFF));
    }

    return true;
}

void stream_rx_process_packet(void)
{
    if (s_packet_bytes[0] != STREAM_MAGIC0 || s_packet_bytes[1] != STREAM_MAGIC1) {
        s_stream_bad_packets++;
        s_payload_errors++;
        return;
    }

    uint16_t sample_count = get_u16_le(&s_packet_bytes[4]);
    uint32_t checksum = get_u32_le(&s_packet_bytes[8]);

    if (sample_count != STREAM_SAMPLES_PER_PACKET) {
        s_stream_bad_packets++;
        s_payload_errors++;
        return;
    }

    const uint8_t *payload = &s_packet_bytes[STREAM_HEADER_BYTES];

    if (checksum32_payload(payload, STREAM_PAYLOAD_BYTES) != checksum) {
        s_stream_bad_packets++;
        s_payload_errors++;
        return;
    }

    /*
       UART commands are handled while a packet is still arriving. A STOP or
       CLEAR that landed mid-transfer already emptied the ring for the
       stream this packet belongs to; queuing it now would make the next
       START play a stale packet, and jump to it from the parked centre.
    */
    if (ring_get_epoch() != s_packet_epoch) {
        s_stale_packets++;
        s_stream_packets++;
        return;
    }

    uint16_t seq = get_u16_le(&s_packet_bytes[2]);

    if (!s_seq_valid || s_seq_epoch != s_packet_epoch) {
        s_seq_valid = true;
        s_seq_epoch = s_packet_epoch;
    } else if (seq != s_seq_expected) {
        s_seq_gaps++;
        s_seq_missing += (uint16_t)(seq - s_seq_expected);
    }
    s_seq_expected = (uint16_t)(seq + 1u);

    const int16_t *samples = (const int16_t *)payload;

    uint32_t pushed = ring_push_block(samples, STREAM_SAMPLES_PER_PACKET);
    if (pushed < STREAM_SAMPLES_PER_PACKET) {
        s_stream_dropped_packets++;
        s_payload_errors++;
    }

    s_stream_packets++;
    s_received_blocks++;
    s_received_samples += pushed;
}

void stream_rx_get_status(stream_rx_status_t *status)
{
    if (!status) {
        return;
    }

    status->payload_errors = s_payload_errors;
    status->received_blocks = s_received_blocks;
    status->received_samples = s_received_samples;
    status->stream_packets = s_stream_packets;
    status->stream_bad_packets = s_stream_bad_packets;
    status->stream_dropped_packets = s_stream_dropped_packets;
    status->stream_rx_timeouts = s_stream_rx_timeouts;
    status->stream_stale_packets = s_stale_packets;
    status->seq_gaps = s_seq_gaps;
    status->seq_missing = s_seq_missing;
    status->rx_partial = s_rx_partial;
}
