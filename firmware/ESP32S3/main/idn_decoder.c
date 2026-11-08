/*
 * idn_decoder.c
 *
 * Tag dictionary compiler and wave sample decoder for the IDN-Stream laser
 * projector service. See idn_decoder.h for the contract and policies.
 * Section numbers refer to ILDA IDN-Stream rev002.
 */

#include <string.h>

#include "idn_decoder.h"

#define TAG_PRECISION        0x4010u
#define TAG_NOP              0x4000u

#define SPACE_FIELD_RESERVED 2u

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static idn_decoder_result_t result(idn_decoder_status_t status, idn_decoder_reason_t reason,
                                   uint16_t tag_index, uint16_t tag)
{
    idn_decoder_result_t r = { status, reason, tag_index, tag };
    return r;
}

void idn_decoder_default_caps(idn_decoder_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->color_count = 3;
    caps->colors[0].wavelength_nm = 638;   /* R, ch[2] */
    caps->colors[0].channel       = 2;
    caps->colors[1].wavelength_nm = 532;   /* G, ch[3] */
    caps->colors[1].channel       = 3;
    caps->colors[2].wavelength_nm = 460;   /* B, ch[4] */
    caps->colors[2].channel       = 4;
    caps->intensity_channel = 5;

    /* Only "projector specific" (0) and "no transformation" (3): nothing is corrected here. */
    caps->space.glin_mask = 0x09;
    caps->space.clin_mask = 0x09;
    caps->space.cbal_mask = 0x09;
    caps->space.ctim_mask = 0x09;
}

static bool caps_valid(const idn_decoder_caps_t *caps)
{
    if (caps->color_count > IDN_DECODER_MAX_COLOR_LINES) return false;

    uint16_t used = (1u << IDN_DECODER_CH_X) | (1u << IDN_DECODER_CH_Y);
    for (uint8_t i = 0; i < caps->color_count; i++) {
        uint8_t ch = caps->colors[i].channel;
        if (ch >= IDN_DECODER_CHANNELS || (used & (1u << ch))) return false;
        used |= (uint16_t)(1u << ch);
    }

    if (caps->intensity_channel != IDN_DECODER_NO_CHANNEL) {
        if (caps->intensity_channel >= IDN_DECODER_CHANNELS || (used & (1u << caps->intensity_channel)))
            return false;
    }
    return true;
}

/* Nearest configured wavelength; a tie goes to the shorter wavelength. */
static int nearest_color_line(const idn_decoder_caps_t *caps, uint16_t wl)
{
    int best = -1;
    uint16_t best_dist = 0;

    for (uint8_t i = 0; i < caps->color_count; i++) {
        uint16_t line = caps->colors[i].wavelength_nm;
        uint16_t dist = line > wl ? (uint16_t)(line - wl) : (uint16_t)(wl - line);
        if (best < 0 || dist < best_dist ||
            (dist == best_dist && line < caps->colors[best].wavelength_nm)) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

static bool space_field_ok(uint8_t value, uint8_t mask)
{
    return (mask & (1u << value)) != 0;
}

static idn_decoder_result_t compile_layout(const uint8_t *dict, size_t dict_len,
                                           const idn_decoder_caps_t *caps,
                                           idn_decoder_layout_t *out)
{
    if ((!dict && dict_len > 0) || !caps_valid(caps))
        return result(IDN_DECODER_ERR_MALFORMED, IDN_DECODER_REASON_BAD_ARGUMENT, 0, 0);
    if (dict_len % 4u != 0)
        return result(IDN_DECODER_ERR_MALFORMED, IDN_DECODER_REASON_DICT_LENGTH, 0, 0);
    if (dict_len > IDN_DECODER_MAX_DICT_BYTES)
        return result(IDN_DECODER_ERR_RESOURCE, IDN_DECODER_REASON_DICT_TOO_LARGE, 0, 0);

    idn_decoder_layout_t lay;
    memset(&lay, 0, sizeof(lay));
    lay.x_field           = -1;
    lay.y_field           = -1;
    lay.intensity_field   = -1;
    lay.hint_field        = -1;
    lay.intensity_channel = caps->intensity_channel;

    const uint16_t words = (uint16_t)(dict_len / 2u);
    uint16_t i = 0;
    int precision_target = -1;   /* field a following precision tag extends */
    bool pending_prefix = false;
    uint16_t prefix_index = 0;

    while (i < words) {
        const uint16_t index = i;
        const uint16_t tag = rd16(dict + 2u * i);
        i++;

        const uint8_t cat = (uint8_t)((tag >> 12) & 0xFu);
        const uint8_t sub = (uint8_t)((tag >>  8) & 0xFu);
        const uint8_t id  = (uint8_t)((tag >>  4) & 0xFu);
        const uint8_t prm = (uint8_t)(tag & 0xFu);

        /* 3.4.9: a wavelength prefix is followed by precision tags, then a colour with wavelength 0. */
        if (pending_prefix && tag != TAG_PRECISION) {
            if (cat == 5 && sub <= 3 && (tag & 0x03FFu) == 0)
                return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_DYNAMIC_WAVELENGTH, prefix_index, 0x5C00u);
            return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_WAVELENGTH_PREFIX, index, tag);
        }

        /* Struct and modifier categories: no sample octet. */
        if (cat == 0) {
            if (sub != 0)
                return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY, index, tag);
            if (id != 0)
                return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_UNKNOWN_MODIFIER, index, tag);
            /* 3.4.2 Void: skip prm suffix words */
            if (prm > words - i)
                return result(IDN_DECODER_ERR_MALFORMED, IDN_DECODER_REASON_TRUNCATED_SUFFIX, index, tag);
            i = (uint16_t)(i + prm);
            precision_target = -1;
            continue;
        }

        if (cat == 1) {
            if (sub == 0) {
                if (id == 0)   /* 3.4.3 Break: a second scan head */
                    return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_BREAK, index, tag);
                return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_UNKNOWN_MODIFIER, index, tag);
            }
            if (sub == 1) {   /* 3.4.4 Space Modifier */
                if (lay.has_space_modifier)
                    return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_SPACE, index, tag);
                uint8_t p = (uint8_t)(tag & 0xFFu);
                uint8_t glin = (uint8_t)(p >> 6), clin = (uint8_t)((p >> 4) & 3u);
                uint8_t cbal = (uint8_t)((p >> 2) & 3u), ctim = (uint8_t)(p & 3u);
                if (glin == SPACE_FIELD_RESERVED || cbal == SPACE_FIELD_RESERVED || ctim == SPACE_FIELD_RESERVED)
                    return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_SPACE_MODIFIER, index, tag);
                if (!space_field_ok(glin, caps->space.glin_mask) || !space_field_ok(clin, caps->space.clin_mask) ||
                    !space_field_ok(cbal, caps->space.cbal_mask) || !space_field_ok(ctim, caps->space.ctim_mask))
                    return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_SPACE_UNSUPPORTED, index, tag);
                lay.has_space_modifier = true;
                lay.space_modifier = p;
                precision_target = -1;
                continue;
            }
            return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY, index, tag);
        }

        if (cat != 4 && cat != 5)
            return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY, index, tag);

        /* 3.4.5 Precision: one more octet of the directly preceding descriptor. */
        if (tag == TAG_PRECISION) {
            if (precision_target < 0)
                return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_PRECISION_PLACEMENT, index, tag);
            idn_decoder_field_t *f = &lay.fields[precision_target];
            if (f->width >= IDN_DECODER_MAX_VALUE_OCTETS)
                return result(IDN_DECODER_ERR_RESOURCE, IDN_DECODER_REASON_PRECISION_LIMIT, index, tag);
            if (lay.stride >= IDN_DECODER_MAX_STRIDE)
                return result(IDN_DECODER_ERR_RESOURCE, IDN_DECODER_REASON_STRIDE_LIMIT, index, tag);
            f->width++;
            lay.stride++;
            continue;
        }

        /* Every remaining tag is a descriptor with one octet. */
        uint8_t kind = IDN_DECODER_FIELD_IGNORED;
        uint8_t channel = IDN_DECODER_NO_CHANNEL;
        uint16_t wavelength = 0;
        bool takes_precision = true;

        if (cat == 4) {
            if (sub == 0) {
                if (tag == TAG_NOP) takes_precision = false;
                /* other identifiers: unknown descriptor, ignored */
            } else if (sub == 1) {
                if (id == 0) {   /* 3.4.6 Hint */
                    if (prm == 1)
                        return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_SAMPLE_SHUTTER, index, tag);
                    if (prm != 0)
                        return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_HINT_FORMAT, index, tag);
                    if (lay.hint_field >= 0)
                        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_HINT, index, tag);
                    kind = IDN_DECODER_FIELD_HINT;
                    takes_precision = false;
                }
            } else if (sub == 2) {   /* 3.4.7 coordinates; parameter 0 is the standard scanner */
                if (id == 0 && prm == 0) {
                    if (lay.x_field >= 0)
                        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_X, index, tag);
                    kind = IDN_DECODER_FIELD_X;
                } else if (id == 1 && prm == 0) {
                    if (lay.y_field >= 0)
                        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_Y, index, tag);
                    kind = IDN_DECODER_FIELD_Y;
                }
                /* additional scanners, Z, unknown identifiers: ignored */
            } else {
                return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY, index, tag);
            }
        } else {   /* cat == 5 */
            if (sub <= 3) {   /* 3.4.8 colour: 10-bit wavelength in nm */
                wavelength = (uint16_t)(tag & 0x03FFu);
                if (wavelength == 0)
                    return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_ZERO_WAVELENGTH, index, tag);
                int line = nearest_color_line(caps, wavelength);
                if (line < 0)
                    return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_NO_COLOR_OUTPUT, index, tag);
                channel = caps->colors[line].channel;
                if (lay.color_channel_mask & (1u << channel))
                    return result(IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_COLOR_CONFLICT, index, tag);
                lay.color_channel_mask |= (uint8_t)(1u << channel);
                kind = IDN_DECODER_FIELD_COLOR;
            } else if (sub == 12) {   /* 3.4.9 attributes */
                if (id == 0 && prm == 0) {
                    pending_prefix = true;
                    prefix_index = index;
                } else if (id == 1 && prm == 0) {
                    if (lay.intensity_field >= 0)
                        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_INTENSITY, index, tag);
                    kind = IDN_DECODER_FIELD_INTENSITY;
                    channel = caps->intensity_channel;
                }
                /* Beam-Brush, unknown identifiers: ignored */
            } else {
                return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY, index, tag);
            }
        }

        if (lay.stride >= IDN_DECODER_MAX_STRIDE)
            return result(IDN_DECODER_ERR_RESOURCE, IDN_DECODER_REASON_STRIDE_LIMIT, index, tag);

        int8_t n = (int8_t)lay.field_count;
        idn_decoder_field_t *f = &lay.fields[lay.field_count++];
        f->kind          = kind;
        f->offset        = lay.stride;
        f->width         = 1;
        f->channel       = channel;
        f->head          = 0;
        f->tag           = tag;
        f->wavelength_nm = wavelength;
        lay.stride++;

        if (kind == IDN_DECODER_FIELD_X)         lay.x_field = n;
        if (kind == IDN_DECODER_FIELD_Y)         lay.y_field = n;
        if (kind == IDN_DECODER_FIELD_HINT)      lay.hint_field = n;
        if (kind == IDN_DECODER_FIELD_INTENSITY) {
            lay.intensity_field = n;
            if (channel == IDN_DECODER_NO_CHANNEL) f->kind = IDN_DECODER_FIELD_IGNORED;
        }

        precision_target = takes_precision ? n : -1;
    }

    if (pending_prefix)
        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_WAVELENGTH_PREFIX, prefix_index, 0x5C00u);
    if (lay.x_field < 0)
        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_X, 0, 0);
    if (lay.y_field < 0)
        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_Y, 0, 0);
    if (lay.color_channel_mask == 0)
        return result(IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_COLOR, 0, 0);

    lay.valid = true;
    *out = lay;
    return result(IDN_DECODER_OK, IDN_DECODER_REASON_NONE, 0, 0);
}

idn_decoder_result_t idn_decoder_compile(const uint8_t *dict, size_t dict_len,
                                         const idn_decoder_caps_t *caps,
                                         idn_decoder_layout_t *out)
{
    if (!caps || !out)
        return result(IDN_DECODER_ERR_MALFORMED, IDN_DECODER_REASON_BAD_ARGUMENT, 0, 0);

    idn_decoder_layout_t lay;
    idn_decoder_result_t r = compile_layout(dict, dict_len, caps, &lay);

    /* Publish all or nothing: a rejected replacement leaves no usable old layout behind. */
    if (r.status == IDN_DECODER_OK) {
        *out = lay;
    } else {
        memset(out, 0, sizeof(*out));
        out->x_field = out->y_field = out->intensity_field = out->hint_field = -1;
        out->intensity_channel = IDN_DECODER_NO_CHANNEL;
    }
    return r;
}

idn_decoder_result_t idn_decoder_check_wave_chunk(const idn_decoder_layout_t *layout,
                                                  const uint8_t *data, size_t data_len,
                                                  uint32_t *sample_count,
                                                  uint32_t *duration_us)
{
    if (!layout || !layout->valid || layout->stride == 0)
        return result(IDN_DECODER_ERR_INVALID_SAMPLES, IDN_DECODER_REASON_INVALID_LAYOUT, 0, 0);
    if (!data || data_len < IDN_DECODER_WAVE_HEADER_BYTES)
        return result(IDN_DECODER_ERR_INVALID_SAMPLES, IDN_DECODER_REASON_CHUNK_HEADER, 0, 0);

    /* 6.1: flags(1) + duration (24-bit, microseconds) */
    uint32_t duration = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    if (duration == 0)
        return result(IDN_DECODER_ERR_INVALID_SAMPLES, IDN_DECODER_REASON_ZERO_DURATION, 0, 0);

    size_t payload = data_len - IDN_DECODER_WAVE_HEADER_BYTES;
    if (payload % layout->stride != 0)
        return result(IDN_DECODER_ERR_INVALID_SAMPLES, IDN_DECODER_REASON_PARTIAL_SAMPLE, 0, 0);

    size_t count = payload / layout->stride;
    if (count < IDN_DECODER_MIN_WAVE_SAMPLES)
        return result(IDN_DECODER_ERR_INVALID_SAMPLES, IDN_DECODER_REASON_TOO_FEW_SAMPLES, 0, 0);

    if (sample_count) *sample_count = (uint32_t)count;
    if (duration_us)  *duration_us  = duration;
    return result(IDN_DECODER_OK, IDN_DECODER_REASON_NONE, 0, 0);
}

int16_t idn_decoder_optical_to_signed(uint16_t value)
{
    return (int16_t)((int32_t)value - 32768);
}

/* Left-aligned 16-bit value, most significant octet first; octets past 16 bits are truncated. */
static inline uint16_t field_value(const uint8_t *sample, const idn_decoder_field_t *f)
{
    uint16_t v = (uint16_t)((uint16_t)sample[f->offset] << 8);
    if (f->width >= 2) v = (uint16_t)(v | sample[f->offset + 1u]);
    return v;
}

static inline int16_t coordinate(uint16_t raw)
{
    if (raw >= 0x8000u)
        return (int16_t)-(int32_t)(0x10000u - raw);
    return (int16_t)raw;
}

void idn_decoder_decode(const idn_decoder_layout_t *layout,
                        const uint8_t *samples, uint32_t count,
                        int16_t out[][IDN_DECODER_CHANNELS])
{
    for (uint32_t s = 0; s < count; s++) {
        const uint8_t *p = samples + (size_t)s * layout->stride;
        int16_t *o = out[s];

        for (int ch = 0; ch < IDN_DECODER_CHANNELS; ch++) o[ch] = INT16_MIN;
        o[IDN_DECODER_CH_X] = 0;
        o[IDN_DECODER_CH_Y] = 0;

        /* 3.4.6 Hint format 0: CSCL bits 7-6, ISCL bits 5-4 */
        unsigned cscl = 0, iscl = 0;
        if (layout->hint_field >= 0) {
            uint8_t h = p[layout->fields[layout->hint_field].offset];
            cscl = (h >> 6) & 3u;
            iscl = (h >> 4) & 3u;
        }

        uint16_t color_max = 0;
        for (uint8_t j = 0; j < layout->field_count; j++) {
            const idn_decoder_field_t *f = &layout->fields[j];
            switch (f->kind) {
                case IDN_DECODER_FIELD_X:
                    o[IDN_DECODER_CH_X] = coordinate(field_value(p, f));
                    break;
                case IDN_DECODER_FIELD_Y:
                    o[IDN_DECODER_CH_Y] = coordinate(field_value(p, f));
                    break;
                case IDN_DECODER_FIELD_COLOR: {
                    uint16_t v = (uint16_t)(field_value(p, f) >> (2u * cscl));
                    if (v > color_max) color_max = v;
                    o[f->channel] = idn_decoder_optical_to_signed(v);
                    break;
                }
                case IDN_DECODER_FIELD_INTENSITY: {
                    uint16_t v = (uint16_t)(field_value(p, f) >> (2u * iscl));
                    o[f->channel] = idn_decoder_optical_to_signed(v);
                    break;
                }
                default:
                    break;
            }
        }

        if (layout->intensity_field < 0 && layout->intensity_channel != IDN_DECODER_NO_CHANNEL)
            o[layout->intensity_channel] = idn_decoder_optical_to_signed(color_max);
    }
}

const char *idn_decoder_status_name(idn_decoder_status_t status)
{
    switch (status) {
        case IDN_DECODER_OK:                  return "ok";
        case IDN_DECODER_ERR_MALFORMED:       return "malformed";
        case IDN_DECODER_ERR_UNSUPPORTED:     return "unsupported";
        case IDN_DECODER_ERR_INVALID_CONFIG:  return "invalid config";
        case IDN_DECODER_ERR_INVALID_SAMPLES: return "invalid samples";
        case IDN_DECODER_ERR_RESOURCE:        return "resource limit";
    }
    return "?";
}

const char *idn_decoder_reason_name(idn_decoder_reason_t reason)
{
    switch (reason) {
        case IDN_DECODER_REASON_NONE:                return "none";
        case IDN_DECODER_REASON_BAD_ARGUMENT:        return "bad argument";
        case IDN_DECODER_REASON_DICT_LENGTH:         return "dictionary length";
        case IDN_DECODER_REASON_TRUNCATED_SUFFIX:    return "truncated suffix";
        case IDN_DECODER_REASON_UNKNOWN_CATEGORY:    return "unknown category";
        case IDN_DECODER_REASON_UNKNOWN_MODIFIER:    return "unknown modifier";
        case IDN_DECODER_REASON_BREAK:               return "multi-head break";
        case IDN_DECODER_REASON_SAMPLE_SHUTTER:      return "sample shutter";
        case IDN_DECODER_REASON_HINT_FORMAT:         return "hint format";
        case IDN_DECODER_REASON_DUPLICATE_HINT:      return "duplicate hint";
        case IDN_DECODER_REASON_DYNAMIC_WAVELENGTH:  return "dynamic wavelength";
        case IDN_DECODER_REASON_WAVELENGTH_PREFIX:   return "wavelength prefix";
        case IDN_DECODER_REASON_ZERO_WAVELENGTH:     return "zero wavelength";
        case IDN_DECODER_REASON_PRECISION_PLACEMENT: return "precision placement";
        case IDN_DECODER_REASON_DUPLICATE_X:         return "duplicate X";
        case IDN_DECODER_REASON_DUPLICATE_Y:         return "duplicate Y";
        case IDN_DECODER_REASON_DUPLICATE_INTENSITY: return "duplicate intensity";
        case IDN_DECODER_REASON_COLOR_CONFLICT:      return "colour conflict";
        case IDN_DECODER_REASON_NO_COLOR_OUTPUT:     return "no colour output";
        case IDN_DECODER_REASON_SPACE_MODIFIER:      return "space modifier";
        case IDN_DECODER_REASON_SPACE_UNSUPPORTED:   return "space unsupported";
        case IDN_DECODER_REASON_DUPLICATE_SPACE:     return "duplicate space";
        case IDN_DECODER_REASON_MISSING_X:           return "missing X";
        case IDN_DECODER_REASON_MISSING_Y:           return "missing Y";
        case IDN_DECODER_REASON_MISSING_COLOR:       return "missing colour";
        case IDN_DECODER_REASON_DICT_TOO_LARGE:      return "dictionary too large";
        case IDN_DECODER_REASON_STRIDE_LIMIT:        return "stride limit";
        case IDN_DECODER_REASON_PRECISION_LIMIT:     return "precision limit";
        case IDN_DECODER_REASON_CHUNK_HEADER:        return "chunk header";
        case IDN_DECODER_REASON_ZERO_DURATION:       return "zero duration";
        case IDN_DECODER_REASON_PARTIAL_SAMPLE:      return "partial sample";
        case IDN_DECODER_REASON_TOO_FEW_SAMPLES:     return "too few samples";
        case IDN_DECODER_REASON_INVALID_LAYOUT:      return "invalid layout";
    }
    return "?";
}
