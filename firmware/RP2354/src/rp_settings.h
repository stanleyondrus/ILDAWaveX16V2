#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RP_SETTINGS_DEFAULT_MAX_RATE_HZ 30000u

typedef struct {
    uint32_t buffer_size_samples;
    uint32_t buffer_high_water_samples;
    uint32_t buffer_low_water_samples;
    bool mirror_x;
    bool mirror_y;
    uint32_t max_rate_hz;   /* RATE requests above this are clamped */
    int32_t  color_shift_us; /* colour channels delayed (+) or advanced (-) against X/Y */
} rp_settings_t;

void rp_settings_load(void);
void rp_settings_get(rp_settings_t *settings);

bool rp_settings_save(const rp_settings_t *settings);
void rp_settings_save_and_reboot(const rp_settings_t *settings);
