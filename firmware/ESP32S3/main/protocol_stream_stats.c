#include "protocol_stream_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static protocol_stream_stats_t s_stats;

void protocol_stream_stats_add_samples(protocol_stream_source_t source, uint32_t samples)
{
    if (samples == 0) {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);

    switch (source) {
    case PROTOCOL_STREAM_SD_ILDA:
        s_stats.sd_ilda_samples += samples;
        break;
    case PROTOCOL_STREAM_IDN:
        s_stats.idn_samples += samples;
        break;
    case PROTOCOL_STREAM_ETHERDREAM:
        s_stats.etherdream_samples += samples;
        break;
    case PROTOCOL_STREAM_ILW:
        s_stats.ilw_samples += samples;
        break;
    default:
        break;
    }

    portEXIT_CRITICAL(&s_stats_mux);
}

void protocol_stream_stats_add_dropped(protocol_stream_source_t source, uint32_t samples)
{
    if (samples == 0) {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);

    switch (source) {
    case PROTOCOL_STREAM_SD_ILDA:
        s_stats.sd_ilda_dropped += samples;
        break;
    case PROTOCOL_STREAM_IDN:
        s_stats.idn_dropped += samples;
        break;
    case PROTOCOL_STREAM_ETHERDREAM:
        s_stats.etherdream_dropped += samples;
        break;
    case PROTOCOL_STREAM_ILW:
        s_stats.ilw_dropped += samples;
        break;
    default:
        break;
    }

    portEXIT_CRITICAL(&s_stats_mux);
}

void protocol_stream_stats_get(protocol_stream_stats_t *stats)
{
    if (!stats) {
        return;
    }

    portENTER_CRITICAL(&s_stats_mux);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}
