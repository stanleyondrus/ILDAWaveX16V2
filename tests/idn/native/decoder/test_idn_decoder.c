/*
 * Host-native tests for firmware/ESP32S3/main/idn_decoder.c.
 *
 * Dictionaries and samples are literal bytes written from ILDA IDN-Stream
 * rev002 (section numbers next to each case); expected outputs are literal
 * RP channel values, not recomputed with the decoder's own arithmetic.
 *
 * RP channel map: ch[0]=X ch[1]=Y ch[2]=R ch[3]=G ch[4]=B ch[5]=I ch[6]=U1 ch[7]=U2.
 */

#include <stdio.h>
#include <string.h>

#include "idn_decoder.h"

static int s_checks;
static int s_failures;

#define CHECK(cond, ...) do {                                        \
        s_checks++;                                                  \
        if (!(cond)) {                                               \
            s_failures++;                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);              \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

#define N(a) (sizeof(a) / sizeof((a)[0]))

static idn_decoder_caps_t s_caps;

/*
   Mirror of signed_to_dac() in firmware/RP2354/src/dac80508.c and
   firmware/ESP32S3/main/rp_manager_v1.c (both static there): raw DAC code
   for an RP sample value.
*/
static uint16_t dac_code(int16_t v)
{
    return (uint16_t)((int32_t)v + 32768);
}

static void expect_channels(const char *name, const int16_t got[IDN_DECODER_CHANNELS],
                            const int16_t exp[IDN_DECODER_CHANNELS])
{
    for (int ch = 0; ch < IDN_DECODER_CHANNELS; ch++)
        CHECK(got[ch] == exp[ch], "%s: ch[%d] = %d, expected %d", name, ch, got[ch], exp[ch]);
}

static bool compile_ok(const char *name, const uint8_t *dict, size_t len, idn_decoder_layout_t *lay)
{
    idn_decoder_result_t r = idn_decoder_compile(dict, len, &s_caps, lay);
    CHECK(r.status == IDN_DECODER_OK && lay->valid, "%s: compile %s/%s",
          name, idn_decoder_status_name(r.status), idn_decoder_reason_name(r.reason));
    return r.status == IDN_DECODER_OK;
}

static void decode_one(const char *name, const uint8_t *dict, size_t len,
                       const uint8_t *sample, const int16_t exp[IDN_DECODER_CHANNELS])
{
    idn_decoder_layout_t lay;
    int16_t out[1][IDN_DECODER_CHANNELS];
    if (!compile_ok(name, dict, len, &lay)) return;
    idn_decoder_decode(&lay, sample, 1, out);
    expect_channels(name, out[0], exp);
}

/* -- Layouts ---------------------------------------------------------------- */

/* 3.4.11 IDTF X/Y/R/G/B: X+precision, Y+precision, 638/532/460 nm, Void pad */
static const uint8_t DICT_RGB16[] = {
    0x42,0x00, 0x40,0x10, 0x42,0x10, 0x40,0x10, 0x52,0x7E, 0x52,0x14, 0x51,0xCC, 0x00,0x00,
};

/* Same colours described blue, green, red */
static const uint8_t DICT_BGR16[] = {
    0x42,0x00, 0x40,0x10, 0x42,0x10, 0x40,0x10, 0x51,0xCC, 0x52,0x14, 0x52,0x7E, 0x00,0x00,
};

/* 3.4.8 green only, 532 nm (0x5214), 8-bit X/Y */
static const uint8_t DICT_GREEN[] = { 0x42,0x00, 0x42,0x10, 0x52,0x14, 0x00,0x00 };

/* 8-bit X/Y/R/G/B with explicit intensity 0x5C10 (3.4.9) */
static const uint8_t DICT_RGBI8[] = { 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x52,0x14, 0x51,0xCC, 0x5C,0x10 };

/* 3.4.5 X with two precision tags (24 bit), 8-bit Y, green */
static const uint8_t DICT_X24[] = { 0x42,0x00, 0x40,0x10, 0x40,0x10, 0x42,0x10, 0x52,0x14, 0x00,0x00 };

/* 3.4.6 hint format 0, X, Y, red, intensity */
static const uint8_t DICT_HINT[] = { 0x41,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x5C,0x10, 0x00,0x00 };

/* -- Output semantics ------------------------------------------------------- */

static void test_rgb_and_order(void)
{
    /* X=0x1234 Y=0xFEDC R=0xFF G=0x80 B=0x00 */
    static const uint8_t s_rgb[] = { 0x12,0x34, 0xFE,0xDC, 0xFF, 0x80, 0x00 };
    static const uint8_t s_bgr[] = { 0x12,0x34, 0xFE,0xDC, 0x00, 0x80, 0xFF };
    static const int16_t exp[8] = { 4660, -292, 32512, 0, -32768, 32512, -32768, -32768 };

    decode_one("rgb16", DICT_RGB16, sizeof(DICT_RGB16), s_rgb, exp);
    decode_one("bgr16", DICT_BGR16, sizeof(DICT_BGR16), s_bgr, exp);

    idn_decoder_layout_t lay;
    if (compile_ok("rgb16 layout", DICT_RGB16, sizeof(DICT_RGB16), &lay)) {
        CHECK(lay.stride == 7, "rgb16 stride %u", lay.stride);
        CHECK(lay.intensity_field < 0 && lay.hint_field < 0, "rgb16 optional fields");
        CHECK(lay.fields[lay.x_field].width == 2, "rgb16 X width");
    }
}

static void test_green_only(void)
{
    static const uint8_t s[] = { 0x00, 0x00, 0xFF };
    static const int16_t exp[8] = { 0, 0, -32768, 32512, -32768, 32512, -32768, -32768 };
    decode_one("green only", DICT_GREEN, sizeof(DICT_GREEN), s, exp);
}

static void test_intensity(void)
{
    /* Explicit intensity zero stays raw zero and leaves RGB as sent. */
    static const uint8_t s_zero[] = { 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00 };
    static const int16_t e_zero[8] = { 0, 0, 32512, 32512, 32512, -32768, -32768, -32768 };
    decode_one("explicit intensity 0", DICT_RGBI8, sizeof(DICT_RGBI8), s_zero, e_zero);

    static const uint8_t s_half[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x80 };
    static const int16_t e_half[8] = { 0, 0, -32768, -32768, -32768, 0, -32768, -32768 };
    decode_one("explicit intensity, colours off", DICT_RGBI8, sizeof(DICT_RGBI8), s_half, e_half);

    /* Absent intensity: largest colour (R=0x10 G=0x40 B=0x20 -> 0x4000). */
    static const uint8_t s_abs[] = { 0x00,0x00, 0x00,0x00, 0x10, 0x40, 0x20 };
    static const int16_t e_abs[8] = { 0, 0, -28672, -16384, -24576, -16384, -32768, -32768 };
    decode_one("absent intensity", DICT_RGB16, sizeof(DICT_RGB16), s_abs, e_abs);
}

static void test_all_zero_dac_codes(void)
{
    static const uint8_t s_i[] = { 0, 0, 0, 0, 0, 0 };
    static const uint8_t s_n[] = { 0, 0, 0, 0, 0, 0, 0 };
    static const int16_t exp[8] = { 0, 0, -32768, -32768, -32768, -32768, -32768, -32768 };
    static const uint16_t codes[8] = { 0x8000, 0x8000, 0, 0, 0, 0, 0, 0 };

    const struct { const char *name; const uint8_t *dict; size_t len; const uint8_t *s; } cases[] = {
        { "all zero, explicit I", DICT_RGBI8, sizeof(DICT_RGBI8), s_i },
        { "all zero, derived I",  DICT_RGB16, sizeof(DICT_RGB16), s_n },
    };

    for (size_t c = 0; c < N(cases); c++) {
        idn_decoder_layout_t lay;
        int16_t out[1][IDN_DECODER_CHANNELS];
        if (!compile_ok(cases[c].name, cases[c].dict, cases[c].len, &lay)) continue;
        idn_decoder_decode(&lay, cases[c].s, 1, out);
        expect_channels(cases[c].name, out[0], exp);
        for (int ch = 0; ch < IDN_DECODER_CHANNELS; ch++)
            CHECK(dac_code(out[0][ch]) == codes[ch], "%s: DAC code ch[%d] = 0x%04X, expected 0x%04X",
                  cases[c].name, ch, dac_code(out[0][ch]), codes[ch]);
    }

    CHECK(idn_decoder_optical_to_signed(0) == -32768, "optical 0");
    CHECK(idn_decoder_optical_to_signed(0xFFFF) == 32767, "optical max");
}

static void test_precision(void)
{
    /* X = 12 34 56 -> 0x1234 (third octet truncated), Y = 0x7F -> 0x7F00 */
    static const uint8_t s[] = { 0x12, 0x34, 0x56, 0x7F, 0xFF };
    static const int16_t exp[8] = { 4660, 32512, -32768, 32512, -32768, 32512, -32768, -32768 };
    decode_one("X 24-bit", DICT_X24, sizeof(DICT_X24), s, exp);

    idn_decoder_layout_t lay;
    if (compile_ok("X 24-bit layout", DICT_X24, sizeof(DICT_X24), &lay)) {
        CHECK(lay.stride == 5, "X24 stride %u", lay.stride);
        CHECK(lay.fields[lay.x_field].width == 3, "X24 width");
        CHECK(lay.fields[lay.y_field].offset == 3, "X24 Y offset");
    }

    /* Signed extremes, 16 bit */
    static const uint8_t s_ext[] = { 0x80,0x00, 0x7F,0xFF, 0, 0, 0 };
    static const int16_t e_ext[8] = { -32768, 32767, -32768, -32768, -32768, -32768, -32768, -32768 };
    decode_one("coordinate extremes", DICT_RGB16, sizeof(DICT_RGB16), s_ext, e_ext);

    static const uint8_t s_m1[] = { 0xFF,0xFF, 0x00,0x01, 0, 0, 0 };
    static const int16_t e_m1[8] = { -1, 1, -32768, -32768, -32768, -32768, -32768, -32768 };
    decode_one("coordinate -1/+1", DICT_RGB16, sizeof(DICT_RGB16), s_m1, e_m1);

    /* 8-bit coordinates are left aligned: 0x80 -> -32768 */
    static const uint8_t s_8[] = { 0x80, 0x01, 0x00 };
    static const int16_t e_8[8] = { -32768, 256, -32768, -32768, -32768, -32768, -32768, -32768 };
    decode_one("8-bit coordinates", DICT_GREEN, sizeof(DICT_GREEN), s_8, e_8);

    /* 16-bit colour precision is kept: R = 0x1234 */
    static const uint8_t d16[] = { 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x40,0x10 };
    static const uint8_t s16[] = { 0x00, 0x00, 0x12, 0x34 };
    static const int16_t e16[8] = { 0, 0, -28108, -32768, -32768, -28108, -32768, -32768 };
    decode_one("16-bit colour", d16, sizeof(d16), s16, e16);
}

static void test_scales(void)
{
    /* 3.4.6: CSCL bits 7-6 and ISCL bits 5-4 divide by 1, 4, 16, 64 (R = I = 0xFF). */
    static const struct { uint8_t hint; int16_t r; int16_t i; } cases[] = {
        { 0x00,  32512,  32512 },
        { 0x40, -16448,  32512 },   /* 0xFF00 / 4  = 0x3FC0 */
        { 0x80, -28688,  32512 },   /* 0xFF00 / 16 = 0x0FF0 */
        { 0xC0, -31748,  32512 },   /* 0xFF00 / 64 = 0x03FC */
        { 0x10,  32512, -16448 },
        { 0x20,  32512, -28688 },
        { 0x30,  32512, -31748 },
        { 0xF0, -31748, -31748 },
        { 0x70, -16448, -31748 },
        { 0x0F,  32512,  32512 },   /* reserved low bits ignored */
    };

    idn_decoder_layout_t lay;
    if (!compile_ok("hint layout", DICT_HINT, sizeof(DICT_HINT), &lay)) return;
    CHECK(lay.stride == 5 && lay.hint_field == 0, "hint layout stride %u", lay.stride);

    for (size_t c = 0; c < N(cases); c++) {
        uint8_t s[5] = { cases[c].hint, 0x00, 0x00, 0xFF, 0xFF };
        int16_t out[1][IDN_DECODER_CHANNELS];
        idn_decoder_decode(&lay, s, 1, out);
        CHECK(out[0][2] == cases[c].r, "hint 0x%02X: R %d, expected %d", cases[c].hint, out[0][2], cases[c].r);
        CHECK(out[0][5] == cases[c].i, "hint 0x%02X: I %d, expected %d", cases[c].hint, out[0][5], cases[c].i);
        CHECK(out[0][3] == -32768 && out[0][6] == -32768, "hint 0x%02X: unassigned", cases[c].hint);
    }

    /* Absent intensity derives from the scaled colour (CSCL 1, R=0xFF -> 0x3FC0). */
    static const uint8_t d[] = { 0x41,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E };
    static const uint8_t s[] = { 0x70, 0x00, 0x00, 0xFF };
    static const int16_t e[8] = { 0, 0, -16448, -32768, -32768, -16448, -32768, -32768 };
    decode_one("derived intensity after CSCL", d, sizeof(d), s, e);
}

static void test_wavelength_mapping(void)
{
    /* 3.4.8 single colour at a given wavelength; which RP line lights */
    static const struct { uint8_t hi, lo; uint16_t nm; int ch; } cases[] = {
        { 0x52, 0x7E, 638, 2 },
        { 0x52, 0x80, 640, 2 },
        { 0x52, 0x14, 532, 3 },
        { 0x52, 0x41, 577, 3 },
        { 0x52, 0x49, 585, 3 },   /* tie 638/532 -> shorter */
        { 0x51, 0xF0, 496, 4 },   /* tie 532/460 -> shorter */
        { 0x51, 0xCC, 460, 4 },
        { 0x51, 0xC2, 450, 4 },
        { 0x51, 0x95, 405, 4 },
    };

    for (size_t c = 0; c < N(cases); c++) {
        uint8_t d[] = { 0x42,0x00, 0x42,0x10, cases[c].hi, cases[c].lo, 0x00,0x00 };
        uint8_t s[] = { 0x00, 0x00, 0xFF };
        idn_decoder_layout_t lay;
        int16_t out[1][IDN_DECODER_CHANNELS];
        char name[32];
        snprintf(name, sizeof(name), "%u nm", cases[c].nm);
        if (!compile_ok(name, d, sizeof(d), &lay)) continue;
        CHECK(lay.fields[2].wavelength_nm == cases[c].nm, "%s: wavelength %u", name, lay.fields[2].wavelength_nm);
        idn_decoder_decode(&lay, s, 1, out);
        for (int ch = 2; ch <= 4; ch++)
            CHECK(out[0][ch] == (ch == cases[c].ch ? 32512 : -32768), "%s: ch[%d] = %d", name, ch, out[0][ch]);
    }

    /* Custom capability: a 445 nm line on U1 takes deep blue. */
    idn_decoder_caps_t caps;
    idn_decoder_default_caps(&caps);
    caps.colors[3].wavelength_nm = 445;
    caps.colors[3].channel = 6;
    caps.color_count = 4;
    static const uint8_t d445[] = { 0x42,0x00, 0x42,0x10, 0x51,0xBD, 0x51,0xCC };
    idn_decoder_layout_t lay;
    idn_decoder_result_t r = idn_decoder_compile(d445, sizeof(d445), &caps, &lay);
    CHECK(r.status == IDN_DECODER_OK, "custom caps compile %s", idn_decoder_reason_name(r.reason));
    if (r.status == IDN_DECODER_OK) {
        static const uint8_t s[] = { 0x00, 0x00, 0xFF, 0x00 };
        static const int16_t e[8] = { 0, 0, -32768, -32768, -32768, 32512, 32512, -32768 };
        int16_t out[1][IDN_DECODER_CHANNELS];
        idn_decoder_decode(&lay, s, 1, out);
        expect_channels("445 nm on U1", out[0], e);
    }
}

static void test_ignored_descriptors(void)
{
    /* X, Y, Z+precision (3.4.7), unknown 0x4020, Beam-Brush 0x5C20, red */
    static const uint8_t d[] = { 0x42,0x00, 0x42,0x10, 0x42,0x20, 0x40,0x10,
                                 0x40,0x20, 0x5C,0x20, 0x52,0x7E, 0x00,0x00 };
    static const uint8_t s[] = { 0x7F, 0x80, 0xAA, 0xBB, 0xCC, 0xDD, 0xFF };
    static const int16_t e[8] = { 32512, -32768, 32512, -32768, -32768, 32512, -32768, -32768 };
    decode_one("ignored descriptors", d, sizeof(d), s, e);

    /* NOP 0x4000 consumes an octet */
    static const uint8_t dn[] = { 0x40,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E };
    static const uint8_t sn[] = { 0x55, 0x01, 0x02, 0xFF };
    static const int16_t en[8] = { 256, 512, 32512, -32768, -32768, 32512, -32768, -32768 };
    decode_one("NOP", dn, sizeof(dn), sn, en);

    /* X-prime 0x4201 + precision (3.4.10 U4) is consumed, not mixed into X */
    static const uint8_t dp[] = { 0x42,0x00, 0x42,0x10, 0x42,0x01, 0x40,0x10, 0x52,0x7E, 0x00,0x00 };
    static const uint8_t sp[] = { 0x01, 0x02, 0x7F, 0xFF, 0xFF };
    static const int16_t ep[8] = { 256, 512, 32512, -32768, -32768, 32512, -32768, -32768 };
    decode_one("X-prime ignored", dp, sizeof(dp), sp, ep);

    /* Void with one suffix word (3.4.2) */
    static const uint8_t dv[] = { 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x01, 0x12,0x34, 0x00,0x00 };
    idn_decoder_layout_t lay;
    if (compile_ok("void suffix", dv, sizeof(dv), &lay))
        CHECK(lay.stride == 3, "void suffix stride %u", lay.stride);

    /* Space modifier settings this target accepts (3.4.4) */
    static const uint8_t ds0[] = { 0x11,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E };
    static const uint8_t ds3[] = { 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x11,0xFF };
    compile_ok("space projector specific", ds0, sizeof(ds0), &lay);
    compile_ok("space no transformation", ds3, sizeof(ds3), &lay);
}

/* -- Configuration rejection ----------------------------------------------- */

typedef struct {
    const char           *name;
    const uint8_t        *dict;
    size_t                len;
    idn_decoder_status_t  status;
    idn_decoder_reason_t  reason;
} reject_case_t;

#define D(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })

static void test_rejections(void)
{
    const reject_case_t cases[] = {
        { "cat 2",              D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x20,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 6",              D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x60,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 12 (dynamic)",   D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0xC0,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 0 sub 1",        D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x01,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 1 sub 2",        D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x12,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 4 sub 3",        D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x43,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "cat 5 sub 4",        D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x54,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_UNKNOWN_CATEGORY },
        { "truncated suffix",   D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x03), IDN_DECODER_ERR_MALFORMED,      IDN_DECODER_REASON_TRUNCATED_SUFFIX },
        { "odd word count",     D(0x42,0x00, 0x42,0x10, 0x52,0x7E),            IDN_DECODER_ERR_MALFORMED,      IDN_DECODER_REASON_DICT_LENGTH },
        { "empty",              NULL, 0,                                        IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_X },
        { "break",              D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x10,0x00, 0x42,0x01, 0x42,0x11, 0x52,0x7E, 0x00,0x00),
                                                                                IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_BREAK },
        { "sample shutter",     D(0x41,0x01, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_SAMPLE_SHUTTER },
        /* 3.4.10 full ISP-DB25 default mapping, then padding */
        { "ISP-DB25 default",   D(0x41,0x01, 0x5C,0x10, 0x52,0x7E, 0x52,0x14, 0x51,0xCC, 0x51,0xBD, 0x52,0x41, 0x51,0xE8,
                                  0x42,0x00, 0x40,0x10, 0x42,0x10, 0x40,0x10, 0x42,0x01, 0x40,0x10, 0x00,0x00, 0x00,0x00),
                                                                                IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_SAMPLE_SHUTTER },
        /* Same without 0x4101: 445 nm lands on the 460 nm line already used */
        { "ISP-DB25, no shutter", D(0x5C,0x10, 0x52,0x7E, 0x52,0x14, 0x51,0xCC, 0x51,0xBD, 0x52,0x41, 0x51,0xE8,
                                  0x42,0x00, 0x40,0x10, 0x42,0x10, 0x40,0x10, 0x42,0x01, 0x40,0x10, 0x00,0x00),
                                                                                IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_COLOR_CONFLICT },
        { "hint format 2",      D(0x41,0x02, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_HINT_FORMAT },
        { "duplicate hint",     D(0x41,0x00, 0x41,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_HINT },
        { "duplicate X",        D(0x42,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_X },
        { "duplicate Y",        D(0x42,0x00, 0x42,0x10, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_Y },
        { "duplicate I",        D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x5C,0x10, 0x5C,0x10, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_INTENSITY },
        { "colour conflict",    D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x52,0x80), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_COLOR_CONFLICT },
        { "missing X",          D(0x42,0x10, 0x52,0x7E),                       IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_X },
        { "missing Y",          D(0x42,0x00, 0x52,0x7E),                       IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_Y },
        { "intensity only",     D(0x42,0x00, 0x42,0x10, 0x5C,0x10, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_COLOR },
        { "X-prime only",       D(0x42,0x01, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_MISSING_X },
        { "precision first",    D(0x40,0x10, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_PRECISION_PLACEMENT },
        { "precision on hint",  D(0x41,0x00, 0x40,0x10, 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_PRECISION_PLACEMENT },
        { "precision on NOP",   D(0x40,0x00, 0x40,0x10, 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_PRECISION_PLACEMENT },
        { "precision after void", D(0x42,0x00, 0x00,0x00, 0x40,0x10, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_PRECISION_PLACEMENT },
        { "precision limit",    D(0x42,0x00, 0x40,0x10, 0x40,0x10, 0x40,0x10, 0x40,0x10, 0x42,0x10, 0x52,0x7E, 0x00,0x00),
                                                                                IDN_DECODER_ERR_RESOURCE,       IDN_DECODER_REASON_PRECISION_LIMIT },
        { "zero wavelength",    D(0x42,0x00, 0x42,0x10, 0x50,0x00, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_ZERO_WAVELENGTH },
        { "dynamic wavelength", D(0x42,0x00, 0x42,0x10, 0x5C,0x00, 0x50,0x00), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_DYNAMIC_WAVELENGTH },
        { "dynamic wl + prec",  D(0x42,0x00, 0x42,0x10, 0x5C,0x00, 0x40,0x10, 0x50,0x00, 0x00,0x00), IDN_DECODER_ERR_UNSUPPORTED, IDN_DECODER_REASON_DYNAMIC_WAVELENGTH },
        { "prefix then Y",      D(0x42,0x00, 0x5C,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_WAVELENGTH_PREFIX },
        { "prefix, static colour", D(0x42,0x00, 0x42,0x10, 0x5C,0x00, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_WAVELENGTH_PREFIX },
        { "prefix at end",      D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x5C,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_WAVELENGTH_PREFIX },
        { "unknown struct tag", D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x10), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_UNKNOWN_MODIFIER },
        { "unknown modifier",   D(0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x10,0x10), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_UNKNOWN_MODIFIER },
        { "space GLIN linear",  D(0x11,0x40, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_SPACE_UNSUPPORTED },
        { "space CLIN visual",  D(0x11,0x20, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_UNSUPPORTED,    IDN_DECODER_REASON_SPACE_UNSUPPORTED },
        { "space GLIN reserved", D(0x11,0x80, 0x42,0x00, 0x42,0x10, 0x52,0x7E), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_SPACE_MODIFIER },
        { "space twice",        D(0x11,0x00, 0x11,0x00, 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x00,0x00), IDN_DECODER_ERR_INVALID_CONFIG, IDN_DECODER_REASON_DUPLICATE_SPACE },
    };

    for (size_t c = 0; c < N(cases); c++) {
        idn_decoder_layout_t lay;
        memset(&lay, 0xA5, sizeof(lay));
        idn_decoder_result_t r = idn_decoder_compile(cases[c].dict, cases[c].len, &s_caps, &lay);
        CHECK(r.status == cases[c].status && r.reason == cases[c].reason,
              "%s: got %s/%s, expected %s/%s", cases[c].name,
              idn_decoder_status_name(r.status), idn_decoder_reason_name(r.reason),
              idn_decoder_status_name(cases[c].status), idn_decoder_reason_name(cases[c].reason));
        CHECK(!lay.valid && lay.stride == 0 && lay.field_count == 0, "%s: layout published", cases[c].name);
    }

    /* Stride limit: X, Y, R plus NOPs. 32 octets accepted, 33 rejected. */
    uint8_t d[2 * 40];
    for (int nops = 29; nops <= 30; nops++) {
        size_t w = 0;
        d[w++] = 0x42; d[w++] = 0x00;
        d[w++] = 0x42; d[w++] = 0x10;
        d[w++] = 0x52; d[w++] = 0x7E;
        for (int k = 0; k < nops; k++) { d[w++] = 0x40; d[w++] = 0x00; }
        while (w % 4) { d[w++] = 0x00; d[w++] = 0x00; }
        idn_decoder_layout_t lay;
        idn_decoder_result_t r = idn_decoder_compile(d, w, &s_caps, &lay);
        if (nops == 29)
            CHECK(r.status == IDN_DECODER_OK && lay.stride == 32, "stride 32 accepted (%s)", idn_decoder_reason_name(r.reason));
        else
            CHECK(r.status == IDN_DECODER_ERR_RESOURCE && r.reason == IDN_DECODER_REASON_STRIDE_LIMIT && !lay.valid,
                  "stride 33 rejected (%s)", idn_decoder_reason_name(r.reason));
    }

    /* Dictionary larger than SCWC can declare */
    static uint8_t big[IDN_DECODER_MAX_DICT_BYTES + 4];
    idn_decoder_layout_t lay;
    idn_decoder_result_t r = idn_decoder_compile(big, sizeof(big), &s_caps, &lay);
    CHECK(r.status == IDN_DECODER_ERR_RESOURCE && r.reason == IDN_DECODER_REASON_DICT_TOO_LARGE, "oversized dictionary");

    /* Offending tag is reported */
    static const uint8_t db[] = { 0x42,0x00, 0x42,0x10, 0x52,0x7E, 0x10,0x00 };
    r = idn_decoder_compile(db, sizeof(db), &s_caps, &lay);
    CHECK(r.tag_index == 3 && r.tag == 0x1000, "break tag index %u tag 0x%04X", r.tag_index, r.tag);
}

/* -- Wave chunks (6.1) ------------------------------------------------------ */

static size_t build_chunk(uint8_t *buf, uint32_t duration, const uint8_t *sample, size_t stride, size_t count)
{
    buf[0] = 0x00;
    buf[1] = (uint8_t)(duration >> 16);
    buf[2] = (uint8_t)(duration >> 8);
    buf[3] = (uint8_t)duration;
    for (size_t i = 0; i < count; i++)
        memcpy(buf + 4 + i * stride, sample, stride);
    return 4 + count * stride;
}

static void test_wave_chunks(void)
{
    static const uint8_t s[] = { 0x12,0x34, 0xFE,0xDC, 0xFF, 0x80, 0x00 };
    static const int16_t exp[8] = { 4660, -292, 32512, 0, -32768, 32512, -32768, -32768 };
    uint8_t buf[4 + 7 * 64 + 8];
    uint32_t n = 0, dur = 0;
    idn_decoder_result_t r;
    idn_decoder_layout_t lay;
    if (!compile_ok("chunk layout", DICT_RGB16, sizeof(DICT_RGB16), &lay)) return;

    size_t len = build_chunk(buf, 675, s, 7, 20);
    r = idn_decoder_check_wave_chunk(&lay, buf, len, &n, &dur);
    CHECK(r.status == IDN_DECODER_OK && n == 20 && dur == 675, "20 samples accepted (%s)", idn_decoder_reason_name(r.reason));
    if (r.status == IDN_DECODER_OK) {
        int16_t out[20][IDN_DECODER_CHANNELS];
        idn_decoder_decode(&lay, buf + 4, n, out);
        expect_channels("chunk sample 0", out[0], exp);
        expect_channels("chunk sample 19", out[19], exp);
    }

    const struct { const char *name; size_t len; uint32_t dur; idn_decoder_reason_t reason; } bad[] = {
        { "19 samples",          4 + 7 * 19,     675, IDN_DECODER_REASON_TOO_FEW_SAMPLES },
        { "malformed last",      4 + 7 * 20 + 1, 675, IDN_DECODER_REASON_PARTIAL_SAMPLE },
        { "missing final octet", 4 + 7 * 21 - 1, 675, IDN_DECODER_REASON_PARTIAL_SAMPLE },
        { "zero duration",       4 + 7 * 20,     0,   IDN_DECODER_REASON_ZERO_DURATION },
        { "short header",        3,              675, IDN_DECODER_REASON_CHUNK_HEADER },
    };
    for (size_t c = 0; c < N(bad); c++) {
        build_chunk(buf, bad[c].dur, s, 7, 21);
        n = 0xDEAD;
        r = idn_decoder_check_wave_chunk(&lay, buf, bad[c].len, &n, NULL);
        CHECK(r.status == IDN_DECODER_ERR_INVALID_SAMPLES && r.reason == bad[c].reason && n == 0xDEAD,
              "%s: got %s/%s", bad[c].name, idn_decoder_status_name(r.status), idn_decoder_reason_name(r.reason));
    }
}

static void test_rejected_replacement(void)
{
    /* Good layout, then a replacement with Break: data in the old format must no longer decode. */
    static const uint8_t s[] = { 0x12,0x34, 0xFE,0xDC, 0xFF, 0x80, 0x00 };
    static const uint8_t brk[] = { 0x42,0x00, 0x40,0x10, 0x42,0x10, 0x40,0x10, 0x52,0x7E, 0x52,0x14, 0x51,0xCC, 0x10,0x00 };
    uint8_t buf[4 + 7 * 20];
    uint32_t n = 0;
    idn_decoder_layout_t lay;

    if (!compile_ok("replacement base", DICT_RGB16, sizeof(DICT_RGB16), &lay)) return;
    size_t len = build_chunk(buf, 675, s, 7, 20);
    CHECK(idn_decoder_check_wave_chunk(&lay, buf, len, &n, NULL).status == IDN_DECODER_OK, "base chunk");

    idn_decoder_result_t r = idn_decoder_compile(brk, sizeof(brk), &s_caps, &lay);
    CHECK(r.status == IDN_DECODER_ERR_UNSUPPORTED && !lay.valid, "replacement rejected");
    r = idn_decoder_check_wave_chunk(&lay, buf, len, &n, NULL);
    CHECK(r.status == IDN_DECODER_ERR_INVALID_SAMPLES && r.reason == IDN_DECODER_REASON_INVALID_LAYOUT,
          "old decoder reused after rejected replacement");
}

int main(void)
{
    idn_decoder_default_caps(&s_caps);

    test_rgb_and_order();
    test_green_only();
    test_intensity();
    test_all_zero_dac_codes();
    test_precision();
    test_scales();
    test_wavelength_mapping();
    test_ignored_descriptors();
    test_rejections();
    test_wave_chunks();
    test_rejected_replacement();

    printf("idn_decoder: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
