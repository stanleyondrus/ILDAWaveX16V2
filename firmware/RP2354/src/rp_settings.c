#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "ring_buffer.h"
#include "rp_settings.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#define SETTINGS_MAGIC   0x49574442u
#define SETTINGS_VERSION 3u
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (2 * FLASH_SECTOR_SIZE))

#define MAX_RATE_MIN_HZ 1000u
#define MAX_RATE_MAX_HZ 200000u
#define COLOR_SHIFT_MAX_US 2000

/* Version 1 record: no rate limit. Still accepted so an upgrade keeps the
   buffer and mirror settings. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size_samples;
    uint32_t buffer_high_water_samples;
    uint32_t buffer_low_water_samples;
    uint8_t  mirror_x;
    uint8_t  mirror_y;
    uint8_t  _pad[2];
    uint32_t checksum;
} settings_record_v1_t;

/* Version 2 record: rate limit, no colour shift. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size_samples;
    uint32_t buffer_high_water_samples;
    uint32_t buffer_low_water_samples;
    uint8_t  mirror_x;
    uint8_t  mirror_y;
    uint8_t  _pad[2];
    uint32_t max_rate_hz;
    uint32_t checksum;
} settings_record_v2_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size_samples;
    uint32_t buffer_high_water_samples;
    uint32_t buffer_low_water_samples;
    uint8_t  mirror_x;
    uint8_t  mirror_y;
    uint8_t  _pad[2];
    uint32_t max_rate_hz;
    int32_t  color_shift_us;
    uint32_t checksum;
} settings_record_t;

static rp_settings_t s_settings = {
    .buffer_size_samples      = RP_RING_SAMPLES,
    .buffer_high_water_samples = RP_RING_HIGH_WATER,
    .buffer_low_water_samples  = RP_RING_LOW_WATER,
    .mirror_x = false,
    .mirror_y = true,
    .max_rate_hz = RP_SETTINGS_DEFAULT_MAX_RATE_HZ,
    .color_shift_us = 0,
};

static const uint8_t *s_flash_record_bytes = (const uint8_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);

typedef struct {
    uint8_t page[FLASH_PAGE_SIZE];
} flash_save_ctx_t;

static uint32_t clamp_capacity(uint32_t value)
{
    if (value < RP_RING_MIN_SAMPLES) {
        return RP_RING_MIN_SAMPLES;
    }

    if (value > RP_RING_SAMPLES) {
        return RP_RING_SAMPLES;
    }

    return value;
}

static uint32_t clamp_high_water(uint32_t capacity, uint32_t value)
{
    uint32_t max_high_water = capacity - RP_RING_PACKET_MARGIN;

    if (value < RP_RING_PACKET_MARGIN) {
        return RP_RING_PACKET_MARGIN;
    }

    if (value > max_high_water) {
        return max_high_water;
    }

    return value;
}

static uint32_t clamp_low_water(uint32_t high_water, uint32_t value)
{
    uint32_t max_low_water = high_water > RP_RING_PACKET_MARGIN ?
                             high_water - RP_RING_PACKET_MARGIN : 0;

    if (value > max_low_water) {
        return max_low_water;
    }

    return value;
}

static uint32_t clamp_max_rate(uint32_t value)
{
    if (value < MAX_RATE_MIN_HZ) {
        return MAX_RATE_MIN_HZ;
    }

    if (value > MAX_RATE_MAX_HZ) {
        return MAX_RATE_MAX_HZ;
    }

    return value;
}

static int32_t clamp_color_shift(int32_t value)
{
    if (value < -COLOR_SHIFT_MAX_US) {
        return -COLOR_SHIFT_MAX_US;
    }

    if (value > COLOR_SHIFT_MAX_US) {
        return COLOR_SHIFT_MAX_US;
    }

    return value;
}

static rp_settings_t normalized_settings(const rp_settings_t *in)
{
    rp_settings_t settings;

    settings.buffer_size_samples = clamp_capacity(in->buffer_size_samples);
    settings.buffer_high_water_samples =
        clamp_high_water(settings.buffer_size_samples, in->buffer_high_water_samples);
    settings.buffer_low_water_samples =
        clamp_low_water(settings.buffer_high_water_samples, in->buffer_low_water_samples);
    settings.mirror_x = in->mirror_x;
    settings.mirror_y = in->mirror_y;
    settings.max_rate_hz = clamp_max_rate(in->max_rate_hz);
    settings.color_shift_us = clamp_color_shift(in->color_shift_us);

    return settings;
}

static uint32_t checksum_fields(uint32_t magic, uint32_t version,
                                uint32_t size, uint32_t high_water, uint32_t low_water,
                                uint8_t mirror_x, uint8_t mirror_y)
{
    return magic ^
           (version * 0x45d9f3bu) ^
           (size * 0x119de1f3u) ^
           (high_water * 0x3449c5abu) ^
           (low_water * 0x27d4eb2du) ^
           ((uint32_t)mirror_x * 0x1b873593u) ^
           ((uint32_t)mirror_y * 0xe654c4ebu);
}

static uint32_t record_checksum_v1(const settings_record_v1_t *r)
{
    return checksum_fields(r->magic, r->version,
                           r->buffer_size_samples, r->buffer_high_water_samples,
                           r->buffer_low_water_samples, r->mirror_x, r->mirror_y);
}

static uint32_t record_checksum_v2(const settings_record_v2_t *r)
{
    return checksum_fields(r->magic, r->version,
                           r->buffer_size_samples, r->buffer_high_water_samples,
                           r->buffer_low_water_samples, r->mirror_x, r->mirror_y) ^
           (r->max_rate_hz * 0x85ebca6bu);
}

static uint32_t record_checksum(const settings_record_t *r)
{
    return checksum_fields(r->magic, r->version,
                           r->buffer_size_samples, r->buffer_high_water_samples,
                           r->buffer_low_water_samples, r->mirror_x, r->mirror_y) ^
           (r->max_rate_hz * 0x85ebca6bu) ^
           ((uint32_t)r->color_shift_us * 0xc2b2ae35u);
}

void rp_settings_load(void)
{
    rp_settings_t loaded = {
        .buffer_size_samples      = RP_RING_SAMPLES,
        .buffer_high_water_samples = RP_RING_HIGH_WATER,
        .buffer_low_water_samples  = RP_RING_LOW_WATER,
        .mirror_x = false,
        .mirror_y = true,
        .max_rate_hz = RP_SETTINGS_DEFAULT_MAX_RATE_HZ,
        .color_shift_us = 0,
    };

    settings_record_t rec;
    settings_record_v2_t rec2;
    settings_record_v1_t rec1;

    memcpy(&rec, s_flash_record_bytes, sizeof(rec));
    memcpy(&rec2, s_flash_record_bytes, sizeof(rec2));
    memcpy(&rec1, s_flash_record_bytes, sizeof(rec1));

    if (rec.magic == SETTINGS_MAGIC &&
        rec.version == SETTINGS_VERSION &&
        rec.checksum == record_checksum(&rec)) {
        loaded.buffer_size_samples       = rec.buffer_size_samples;
        loaded.buffer_high_water_samples = rec.buffer_high_water_samples;
        loaded.buffer_low_water_samples  = rec.buffer_low_water_samples;
        loaded.mirror_x = rec.mirror_x != 0;
        loaded.mirror_y = rec.mirror_y != 0;
        loaded.max_rate_hz = rec.max_rate_hz;
        loaded.color_shift_us = rec.color_shift_us;
    } else if (rec2.magic == SETTINGS_MAGIC &&
               rec2.version == 2u &&
               rec2.checksum == record_checksum_v2(&rec2)) {
        loaded.buffer_size_samples       = rec2.buffer_size_samples;
        loaded.buffer_high_water_samples = rec2.buffer_high_water_samples;
        loaded.buffer_low_water_samples  = rec2.buffer_low_water_samples;
        loaded.mirror_x = rec2.mirror_x != 0;
        loaded.mirror_y = rec2.mirror_y != 0;
        loaded.max_rate_hz = rec2.max_rate_hz;
    } else if (rec1.magic == SETTINGS_MAGIC &&
               rec1.version == 1u &&
               rec1.checksum == record_checksum_v1(&rec1)) {
        /* Upgrade from a record without a rate limit: keep the default limit. */
        loaded.buffer_size_samples       = rec1.buffer_size_samples;
        loaded.buffer_high_water_samples = rec1.buffer_high_water_samples;
        loaded.buffer_low_water_samples  = rec1.buffer_low_water_samples;
        loaded.mirror_x = rec1.mirror_x != 0;
        loaded.mirror_y = rec1.mirror_y != 0;
    }

    s_settings = normalized_settings(&loaded);
}

void rp_settings_get(rp_settings_t *settings)
{
    if (!settings) {
        return;
    }

    *settings = s_settings;
}

static void flash_save_execute(void *param)
{
    flash_save_ctx_t *ctx = (flash_save_ctx_t *)param;

    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, ctx->page, sizeof(ctx->page));
}

bool rp_settings_save(const rp_settings_t *settings)
{
    flash_save_ctx_t ctx;
    settings_record_t record;

    if (!settings) {
        return false;
    }

    s_settings = normalized_settings(settings);

    memset(&ctx, 0xff, sizeof(ctx));
    memset(&record, 0, sizeof(record));

    record.magic                    = SETTINGS_MAGIC;
    record.version                  = SETTINGS_VERSION;
    record.buffer_size_samples      = s_settings.buffer_size_samples;
    record.buffer_high_water_samples = s_settings.buffer_high_water_samples;
    record.buffer_low_water_samples  = s_settings.buffer_low_water_samples;
    record.mirror_x                 = s_settings.mirror_x ? 1u : 0u;
    record.mirror_y                 = s_settings.mirror_y ? 1u : 0u;
    record.max_rate_hz              = s_settings.max_rate_hz;
    record.color_shift_us           = s_settings.color_shift_us;
    record.checksum                 = record_checksum(&record);

    memcpy(ctx.page, &record, sizeof(record));

    return flash_safe_execute(flash_save_execute, &ctx, 1000) == PICO_OK;
}

void rp_settings_save_and_reboot(const rp_settings_t *settings)
{
    if (!rp_settings_save(settings)) {
        return;
    }

    watchdog_reboot(0, 0, 0);

    for (;;) {
        tight_loop_contents();
    }
}
