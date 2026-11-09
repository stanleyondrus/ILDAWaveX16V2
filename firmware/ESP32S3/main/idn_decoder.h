#pragma once

/*
 * idn_decoder.h
 *
 * Pure-C IDN-Stream laser graphic decoder (ILDA IDN-Stream rev002, 3.4 and 6.1).
 * No sockets, RTOS, clocks or DAC access: the caller owns channel state and
 * decides what a rejection means for the session.
 *
 * Contract:
 *   - idn_decoder_compile() turns a service configuration (tag dictionary)
 *     into an immutable layout, or returns a typed error. The layout is built
 *     privately and published only when every tag has been checked; on any
 *     error *out is reset to an invalid layout, so a rejected replacement can
 *     never fall back to the previous interpretation or to a default.
 *   - idn_decoder_check_wave_chunk() validates a whole wave chunk (header,
 *     non-zero duration, exact stride, >= 20 samples) before any sample is
 *     decoded. Once it succeeds, idn_decoder_decode() cannot fail.
 *
 * Policies (recorded here so they are deliberate, not accidental):
 *   - Unspecified category/subcategory combinations, including the reserved
 *     dynamic categories 12..15, reject the configuration (3.4).
 *   - Unknown identifiers inside a defined descriptor combination (categories
 *     4 and 5) consume exactly one octet and are ignored, as are precision
 *     octets that extend them. Unknown modifier identifiers (categories 0, 1)
 *     reject, since they may change how the rest is interpreted.
 *   - X/Y with a non-zero parameter (additional scanners), Z and Beam-Brush
 *     are consumed and ignored; this target has no output for them.
 *   - Break (multi-head), Hint format 1 (sample-controlled shutter, 0x4101),
 *     wavelength prefixes, unsupported Space Modifier settings and two colour
 *     tags that map to the same output line are rejected as unsupported.
 *   - X, Y and at least one colour are mandatory; an intensity-only layout is
 *     not accepted.
 *   - Values are assembled most significant octet first and the top 16 bits
 *     kept (wider values are truncated, 8-bit values get zero low bits).
 *   - CSCL/ISCL shift the 16-bit left-aligned value right by 2*scale bits
 *     (truncating), once, before the output conversion.
 *   - Explicit intensity is used as sent, including zero. Absent intensity is
 *     derived as the largest decoded colour value (legacy ISP blanking).
 *   - Colour tags map to the nearest configured output wavelength; a tie goes
 *     to the shorter wavelength.
 *   - Output lines without a descriptor are INT16_MIN (raw DAC code 0).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define IDN_DECODER_CHANNELS          8     /* matches RP_SAMPLE_CHANNELS */
#define IDN_DECODER_CH_X              0
#define IDN_DECODER_CH_Y              1
#define IDN_DECODER_NO_CHANNEL        0xFFu

#define IDN_DECODER_MAX_DICT_BYTES    1020u /* SCWC is 8 bits: 255 words */
#define IDN_DECODER_MAX_STRIDE        32u   /* octets per sample */
#define IDN_DECODER_MAX_VALUE_OCTETS  4u    /* descriptor plus three precision tags */
#define IDN_DECODER_MAX_COLOR_LINES   6u
#define IDN_DECODER_WAVE_HEADER_BYTES 4u
#define IDN_DECODER_MIN_WAVE_SAMPLES  20u   /* IDN-Stream 6.1 */

typedef enum {
    IDN_DECODER_OK = 0,
    IDN_DECODER_ERR_MALFORMED,       /* structure: lengths, suffixes */
    IDN_DECODER_ERR_UNSUPPORTED,     /* valid per standard, not supported here */
    IDN_DECODER_ERR_INVALID_CONFIG,  /* violates the tag rules */
    IDN_DECODER_ERR_INVALID_SAMPLES, /* chunk does not match the layout */
    IDN_DECODER_ERR_RESOURCE,        /* exceeds an implementation limit */
} idn_decoder_status_t;

typedef enum {
    IDN_DECODER_REASON_NONE = 0,
    IDN_DECODER_REASON_BAD_ARGUMENT,
    IDN_DECODER_REASON_DICT_LENGTH,          /* not whole 32-bit words */
    IDN_DECODER_REASON_TRUNCATED_SUFFIX,
    IDN_DECODER_REASON_UNKNOWN_CATEGORY,     /* unspecified CAT/SUB combination */
    IDN_DECODER_REASON_UNKNOWN_MODIFIER,
    IDN_DECODER_REASON_BREAK,                /* second scan head */
    IDN_DECODER_REASON_SAMPLE_SHUTTER,       /* hint format 1, 0x4101 */
    IDN_DECODER_REASON_HINT_FORMAT,
    IDN_DECODER_REASON_DUPLICATE_HINT,
    IDN_DECODER_REASON_DYNAMIC_WAVELENGTH,
    IDN_DECODER_REASON_WAVELENGTH_PREFIX,    /* prefix/colour relationship broken */
    IDN_DECODER_REASON_ZERO_WAVELENGTH,      /* colour 0 nm without prefix */
    IDN_DECODER_REASON_PRECISION_PLACEMENT,
    IDN_DECODER_REASON_DUPLICATE_X,
    IDN_DECODER_REASON_DUPLICATE_Y,
    IDN_DECODER_REASON_DUPLICATE_INTENSITY,
    IDN_DECODER_REASON_COLOR_CONFLICT,       /* two tags on one output line */
    IDN_DECODER_REASON_NO_COLOR_OUTPUT,
    IDN_DECODER_REASON_SPACE_MODIFIER,       /* reserved setting */
    IDN_DECODER_REASON_SPACE_UNSUPPORTED,
    IDN_DECODER_REASON_DUPLICATE_SPACE,
    IDN_DECODER_REASON_MISSING_X,
    IDN_DECODER_REASON_MISSING_Y,
    IDN_DECODER_REASON_MISSING_COLOR,
    IDN_DECODER_REASON_DICT_TOO_LARGE,
    IDN_DECODER_REASON_STRIDE_LIMIT,
    IDN_DECODER_REASON_PRECISION_LIMIT,
    IDN_DECODER_REASON_CHUNK_HEADER,
    IDN_DECODER_REASON_ZERO_DURATION,
    IDN_DECODER_REASON_PARTIAL_SAMPLE,
    IDN_DECODER_REASON_TOO_FEW_SAMPLES,
    IDN_DECODER_REASON_INVALID_LAYOUT,
} idn_decoder_reason_t;

typedef struct {
    idn_decoder_status_t status;
    idn_decoder_reason_t reason;
    uint16_t             tag_index;  /* 16-bit word index of the offending tag */
    uint16_t             tag;        /* offending tag word, 0 when not tag related */
} idn_decoder_result_t;

typedef struct {
    uint16_t wavelength_nm;
    uint8_t  channel;
} idn_decoder_color_line_t;

/* Space Modifier settings this target honours: bit n set = field value n accepted. */
typedef struct {
    uint8_t glin_mask;
    uint8_t clin_mask;
    uint8_t cbal_mask;
    uint8_t ctim_mask;
} idn_decoder_space_caps_t;

typedef struct {
    uint8_t                  color_count;
    idn_decoder_color_line_t colors[IDN_DECODER_MAX_COLOR_LINES];
    uint8_t                  intensity_channel;  /* IDN_DECODER_NO_CHANNEL: none */
    idn_decoder_space_caps_t space;
} idn_decoder_caps_t;

typedef enum {
    IDN_DECODER_FIELD_IGNORED = 0,  /* NOP, unknown descriptor, Z, Beam-Brush, ... */
    IDN_DECODER_FIELD_X,
    IDN_DECODER_FIELD_Y,
    IDN_DECODER_FIELD_COLOR,
    IDN_DECODER_FIELD_INTENSITY,
    IDN_DECODER_FIELD_HINT,
} idn_decoder_field_kind_t;

typedef struct {
    uint8_t  kind;           /* idn_decoder_field_kind_t */
    uint8_t  offset;         /* first octet in the sample */
    uint8_t  width;          /* descriptor octet plus precision octets */
    uint8_t  channel;        /* output line for colour/intensity */
    uint8_t  head;           /* scan head; always 0 (Break is rejected) */
    uint16_t tag;            /* descriptor tag */
    uint16_t wavelength_nm;  /* colour only */
} idn_decoder_field_t;

typedef struct {
    bool                valid;
    uint8_t             stride;
    uint8_t             field_count;
    int8_t              x_field;
    int8_t              y_field;
    int8_t              intensity_field;   /* -1: absent, derived from colours */
    int8_t              hint_field;        /* -1: absent, scales 0 */
    bool                has_space_modifier;
    uint8_t             space_modifier;
    uint8_t             intensity_channel;
    uint8_t             color_channel_mask;
    idn_decoder_field_t fields[IDN_DECODER_MAX_STRIDE];
} idn_decoder_layout_t;

/* RGB lines at the ISP-DB25 default wavelengths (Stream 3.4.10) on the RP channel map. */
void idn_decoder_default_caps(idn_decoder_caps_t *caps);

idn_decoder_result_t idn_decoder_compile(const uint8_t *dict, size_t dict_len,
                                         const idn_decoder_caps_t *caps,
                                         idn_decoder_layout_t *out);

/* data points at the wave chunk header (flags + 24-bit duration). */
idn_decoder_result_t idn_decoder_check_wave_chunk(const idn_decoder_layout_t *layout,
                                                  const uint8_t *data, size_t data_len,
                                                  uint32_t *sample_count,
                                                  uint32_t *duration_us);

/* Decodes count whole samples; callers validate the chunk first. */
void idn_decoder_decode(const idn_decoder_layout_t *layout,
                        const uint8_t *samples, uint32_t count,
                        int16_t out[][IDN_DECODER_CHANNELS]);

/* Unsigned left-aligned optical value -> offset signed RP sample (0 -> INT16_MIN). */
int16_t idn_decoder_optical_to_signed(uint16_t value);

const char *idn_decoder_status_name(idn_decoder_status_t status);
const char *idn_decoder_reason_name(idn_decoder_reason_t reason);
