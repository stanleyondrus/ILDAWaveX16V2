#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RP_SAMPLE_CHANNELS 8
#define RP_RING_SAMPLES 16384
#define RP_RING_HIGH_WATER 14336
#define RP_RING_LOW_WATER 12288
#define RP_RING_MIN_SAMPLES 512
#define RP_RING_PACKET_MARGIN 256

void ring_init(void);
void ring_clear(void);
void ring_configure(uint32_t capacity_samples,
                    uint32_t high_water_samples,
                    uint32_t low_water_samples);

bool ring_push(const int16_t *sample);
uint32_t ring_push_block(const int16_t *samples, uint32_t count);
bool ring_pop(int16_t *sample);
bool ring_peek(int16_t *sample);
uint32_t ring_pop_block(int16_t samples[][RP_SAMPLE_CHANNELS], uint32_t count);

uint32_t ring_get_fill(void);
uint32_t ring_get_capacity(void);
uint32_t ring_get_high_water(void);
uint32_t ring_get_low_water(void);
/* Incremented by every ring_clear() and ring_configure(). */
uint32_t ring_get_epoch(void);
