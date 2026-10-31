#include <string.h>

#include "pico/sync.h"

#include "ring_buffer.h"

static int16_t s_ring[RP_RING_SAMPLES][RP_SAMPLE_CHANNELS];

static volatile uint32_t s_head = 0;
static volatile uint32_t s_tail = 0;
static volatile uint32_t s_fill = 0;
static volatile uint32_t s_capacity = RP_RING_SAMPLES;
static volatile uint32_t s_high_water = RP_RING_HIGH_WATER;
static volatile uint32_t s_low_water = RP_RING_LOW_WATER;
static volatile uint32_t s_epoch = 0;

static critical_section_t s_ring_cs;

static uint32_t clamp_capacity(uint32_t capacity_samples)
{
    if (capacity_samples < RP_RING_MIN_SAMPLES) {
        return RP_RING_MIN_SAMPLES;
    }

    if (capacity_samples > RP_RING_SAMPLES) {
        return RP_RING_SAMPLES;
    }

    return capacity_samples;
}

static uint32_t clamp_high_water(uint32_t capacity_samples, uint32_t high_water_samples)
{
    uint32_t max_high_water = capacity_samples - RP_RING_PACKET_MARGIN;

    if (high_water_samples < RP_RING_PACKET_MARGIN) {
        return RP_RING_PACKET_MARGIN;
    }

    if (high_water_samples > max_high_water) {
        return max_high_water;
    }

    return high_water_samples;
}

static uint32_t clamp_low_water(uint32_t high_water_samples, uint32_t low_water_samples)
{
    uint32_t max_low_water = high_water_samples > RP_RING_PACKET_MARGIN ?
                             high_water_samples - RP_RING_PACKET_MARGIN : 0;

    if (low_water_samples > max_low_water) {
        return max_low_water;
    }

    return low_water_samples;
}

void ring_init(void)
{
    critical_section_init(&s_ring_cs);

    critical_section_enter_blocking(&s_ring_cs);
    s_head = 0;
    s_tail = 0;
    s_fill = 0;
    s_capacity = RP_RING_SAMPLES;
    s_high_water = RP_RING_HIGH_WATER;
    s_low_water = RP_RING_LOW_WATER;
    critical_section_exit(&s_ring_cs);
}

void ring_clear(void)
{
    critical_section_enter_blocking(&s_ring_cs);
    s_head = 0;
    s_tail = 0;
    s_fill = 0;
    s_epoch++;
    critical_section_exit(&s_ring_cs);
}

uint32_t ring_get_epoch(void)
{
    return s_epoch;
}

void ring_configure(uint32_t capacity_samples,
                    uint32_t high_water_samples,
                    uint32_t low_water_samples)
{
    uint32_t capacity = clamp_capacity(capacity_samples);
    uint32_t high_water = clamp_high_water(capacity, high_water_samples);
    uint32_t low_water = clamp_low_water(high_water, low_water_samples);

    critical_section_enter_blocking(&s_ring_cs);
    s_head = 0;
    s_tail = 0;
    s_fill = 0;
    s_capacity = capacity;
    s_high_water = high_water;
    s_low_water = low_water;
    s_epoch++;
    critical_section_exit(&s_ring_cs);
}

bool ring_push(const int16_t *sample)
{
    bool ok = false;

    critical_section_enter_blocking(&s_ring_cs);

    if (s_fill < s_capacity) {
        memcpy(s_ring[s_head], sample, RP_SAMPLE_CHANNELS * sizeof(int16_t));

        s_head++;
        if (s_head >= s_capacity) {
            s_head = 0;
        }

        s_fill++;
        ok = true;
    }

    critical_section_exit(&s_ring_cs);

    return ok;
}

uint32_t ring_push_block(const int16_t *samples, uint32_t count)
{
    if (!samples || count == 0) {
        return 0;
    }

    critical_section_enter_blocking(&s_ring_cs);

    uint32_t free_space = s_capacity - s_fill;
    if (count > free_space) {
        count = free_space;
    }

    uint32_t first = count;
    if (first > s_capacity - s_head) {
        first = s_capacity - s_head;
    }

    if (first > 0) {
        memcpy(s_ring[s_head], samples, first * RP_SAMPLE_CHANNELS * sizeof(int16_t));
    }

    uint32_t second = count - first;
    if (second > 0) {
        memcpy(s_ring[0],
               samples + first * RP_SAMPLE_CHANNELS,
               second * RP_SAMPLE_CHANNELS * sizeof(int16_t));
    }

    s_head += count;
    if (s_head >= s_capacity) {
        s_head -= s_capacity;
    }

    s_fill += count;

    critical_section_exit(&s_ring_cs);

    return count;
}

bool ring_pop(int16_t *sample)
{
    bool ok = false;

    critical_section_enter_blocking(&s_ring_cs);

    if (s_fill > 0) {
        memcpy(sample, s_ring[s_tail], RP_SAMPLE_CHANNELS * sizeof(int16_t));

        s_tail++;
        if (s_tail >= s_capacity) {
            s_tail = 0;
        }

        s_fill--;
        ok = true;
    }

    critical_section_exit(&s_ring_cs);

    return ok;
}

bool ring_peek(int16_t *sample)
{
    bool ok = false;

    critical_section_enter_blocking(&s_ring_cs);

    if (s_fill > 0) {
        memcpy(sample, s_ring[s_tail], RP_SAMPLE_CHANNELS * sizeof(int16_t));
        ok = true;
    }

    critical_section_exit(&s_ring_cs);

    return ok;
}

uint32_t ring_pop_block(int16_t samples[][RP_SAMPLE_CHANNELS], uint32_t count)
{
    if (!samples || count == 0) {
        return 0;
    }

    critical_section_enter_blocking(&s_ring_cs);

    if (count > s_fill) {
        count = s_fill;
    }

    uint32_t first = count;
    if (first > s_capacity - s_tail) {
        first = s_capacity - s_tail;
    }

    if (first > 0) {
        memcpy(samples, s_ring[s_tail], first * RP_SAMPLE_CHANNELS * sizeof(int16_t));
    }

    uint32_t second = count - first;
    if (second > 0) {
        memcpy(samples + first, s_ring[0], second * RP_SAMPLE_CHANNELS * sizeof(int16_t));
    }

    s_tail += count;
    if (s_tail >= s_capacity) {
        s_tail -= s_capacity;
    }

    s_fill -= count;

    critical_section_exit(&s_ring_cs);

    return count;
}

uint32_t ring_get_fill(void)
{
    uint32_t fill;

    critical_section_enter_blocking(&s_ring_cs);
    fill = s_fill;
    critical_section_exit(&s_ring_cs);

    return fill;
}

uint32_t ring_get_capacity(void)
{
    uint32_t capacity;

    critical_section_enter_blocking(&s_ring_cs);
    capacity = s_capacity;
    critical_section_exit(&s_ring_cs);

    return capacity;
}

uint32_t ring_get_high_water(void)
{
    uint32_t high_water;

    critical_section_enter_blocking(&s_ring_cs);
    high_water = s_high_water;
    critical_section_exit(&s_ring_cs);

    return high_water;
}

uint32_t ring_get_low_water(void)
{
    uint32_t low_water;

    critical_section_enter_blocking(&s_ring_cs);
    low_water = s_low_water;
    critical_section_exit(&s_ring_cs);

    return low_water;
}
