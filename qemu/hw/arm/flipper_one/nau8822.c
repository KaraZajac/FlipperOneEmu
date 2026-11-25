/*
 * Nuvoton NAU8822: the Flipper One's audio codec, on the RK3576's I2C6 at
 * 0x1a, as Linux's nau8822 drives it
 *
 * - I2C: a write is two bytes, the register (7 bits) and the 9-bit value's
 *   top bit, then its low 8 bits. A write to R0 resets every register to
 *   its default. Linux never reads it; a read, as the datasheet has it (the
 *   register written, then a repeated start), gives the value's top bit,
 *   then its low 8 bits.
 * - Its clock: as the I2S link's master (R6's CLKIOEN) it makes the frame
 *   clock, IMCLK / 256: IMCLK is MCLK (a property: 12.288 MHz, as the
 *   board's CRU gives it), or its PLL's output, MCLK (halved with PLLMCLK)
 *   × N.K / 4, either divided by MCLKSEL. That rate goes to the SAI on the
 *   link (the board's lrck callback), and Linux sets it to 48 kHz.
 * - Its DAC: the frames the SAI sends (nau8822_dac) go to QEMU's audio, at
 *   that rate, S16 stereo, through a ring of 200 ms. They are silent while
 *   the DAC is soft-muted (R10), powered off (R3) or at volume 0 (R11,
 *   R12), or while neither output it reaches, the headphones (R2, R52,
 *   R53) and the speaker (R3, R54, R55), is on and unmuted; else they have
 *   the DAC's digital volume and the louder output's. The voice is active
 *   while frames come, and for 200 ms after the last.
 * - Its ADC: the frames the SAI receives (nau8822_adc). The left one is the
 *   board's internal microphone on LMICP/LMICN: a WAV file (the "mic"
 *   property: 16-bit PCM, its first channel, any rate, looped), taken as
 *   what the ADC gives at the codec's defaults (the input PGA at 0 dB, its
 *   +20 dB boost, the ADC at 0 dB), then with the PGA's gain, the boost and
 *   the ADC's volume as set (R45, R47, R15). It is silent while that path
 *   is powered off (R2: the PGA, the boost stage, the ADC), not connected
 *   (R44), muted (R45) or at volume 0, and with no file. It starts from the
 *   file's beginning whenever the left ADC is powered on. The right one,
 *   the headset's microphone on RMICP, is silent.
 * The rest (the line and auxiliary inputs, EQ, ALC, the limiter, the
 * high-pass filter) is registers only.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "qemu/audio.h"
#include "flipper_one.h"

#define NAU_REGS        0x50
#define RING            9600            /* frames: 200 ms at 48 kHz */
#define IDLE_NS         200000000       /* the voice stays on, after */

#define R_PM1           0x01
#define R_PM2           0x02
#define R_PM3           0x03
#define R_CLOCKING      0x06
#define R_DAC_CTRL      0x0a
#define R_DAC_VOL_L     0x0b
#define R_DAC_VOL_R     0x0c
#define R_ADC_CTRL      0x0e
#define R_ADC_VOL_L     0x0f
#define R_INPUT         0x2c
#define R_PGA_L         0x2d
#define R_BOOST_L       0x2f
#define R_PLL_N         0x24
#define R_PLL_K1        0x25
#define R_PLL_K2        0x26
#define R_PLL_K3        0x27
#define R_HP_VOL_L      0x34
#define R_HP_VOL_R      0x35
#define R_SPK_VOL_L     0x36
#define R_SPK_VOL_R     0x37
#define R_REVISION      0x3e
#define R_ID            0x3f

OBJECT_DECLARE_SIMPLE_TYPE(Nau8822State, NAU8822)

struct Nau8822State {
    I2CSlave parent_obj;
    uint16_t regs[NAU_REGS];
    uint8_t buf[2];                     /* a write, as it comes */
    int n;
    uint8_t ptr;                        /* a read: its register, */
    int rd;                             /* ... the byte of it next */
    uint32_t mclk;
    int fs;                             /* its frame clock (0: none) */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    int voice_fs;                       /* the voice's rate */
    bool active;
    int16_t ring[2 * RING];             /* frames for QEMU's audio */
    int head, count;
    int64_t last_ns;                    /* frames last came */
    void (*lrck)(void *opaque, int hz);
    void *lrck_opaque;
    char *mic_path;                     /* the internal microphone's file */
    int16_t *mic;                       /* ... its samples */
    size_t mic_n;
    uint32_t mic_rate;
    double mic_pos;                     /* ... where it is in them */
};

/* the driver's defaults (nau8822.c), which a reset gives */
static const uint16_t nau_defaults[NAU_REGS] = {
    [0x04] = 0x050, [0x06] = 0x140, [0x0b] = 0x0ff, [0x0c] = 0x0ff,
    [0x0e] = 0x100, [0x0f] = 0x0ff, [0x10] = 0x0ff, [0x12] = 0x12c,
    [0x13] = 0x02c, [0x14] = 0x02c, [0x15] = 0x02c, [0x16] = 0x02c,
    [0x18] = 0x032, [0x20] = 0x038, [0x21] = 0x00b, [0x22] = 0x032,
    [0x23] = 0x010, [0x24] = 0x008, [0x25] = 0x00c, [0x26] = 0x093,
    [0x27] = 0x0e9, [0x2c] = 0x033, [0x2d] = 0x010, [0x2e] = 0x010,
    [0x2f] = 0x100, [0x30] = 0x100, [0x31] = 0x002, [0x32] = 0x001,
    [0x33] = 0x001, [0x34] = 0x039, [0x35] = 0x039, [0x36] = 0x039,
    [0x37] = 0x039, [0x38] = 0x001, [0x39] = 0x001, [0x3c] = 0x020,
    [0x3e] = 0x07f, [0x3f] = 0x01a, [0x41] = 0x114, [0x48] = 0x008,
};

/* ---------------------------------------------------------- the clock */

/* the frame clock it makes, as master (0: none) */
static int nau_fs(Nau8822State *s)
{
    static const int scaler10[8] = { 10, 15, 20, 30, 40, 60, 80, 120 };
    uint16_t clk = s->regs[R_CLOCKING];
    uint64_t f = s->mclk, div = (uint64_t)scaler10[(clk >> 5) & 7] * 256;

    if (!(clk & 1)) {
        return 0;                       /* a slave: none of its own */
    }
    if (clk & 0x100) {                  /* from its PLL */
        uint64_t nk = (uint64_t)(s->regs[R_PLL_N] & 0xf) << 24 |
                      (s->regs[R_PLL_K1] & 0x3f) << 18 |
                      (s->regs[R_PLL_K2] & 0x1ff) << 9 |
                      (s->regs[R_PLL_K3] & 0x1ff);

        if (!(s->regs[R_PM1] & BIT(5))) {
            return 0;                   /* (its PLL off) */
        }
        f = ((f >> ((s->regs[R_PLL_N] >> 4) & 1)) * nk) >> 24;
        div *= 4;
    }
    return (f * 10 + div / 2) / div;
}

static void nau_audio_out(void *opaque, int free_b);

static void nau_clock_update(Nau8822State *s)
{
    int fs = nau_fs(s);

    if (fs == s->fs) {
        return;
    }
    s->fs = fs;
    if (s->audio_be && fs && fs != s->voice_fs) {
        struct audsettings as = {
            .freq = fs, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        if (s->voice && s->active) {
            audio_be_set_active_out(s->audio_be, s->voice, false);
        }
        s->active = false;
        s->head = s->count = 0;
        s->voice = audio_be_open_out(s->audio_be, s->voice, "nau8822.dac",
                                     s, nau_audio_out, &as);
        s->voice_fs = fs;
    }
    if (s->lrck) {
        s->lrck(s->lrck_opaque, fs);
    }
}

/* ------------------------------------------------------------ the DAC */

/* a channel's gain (1.0: 65536): its DAC's volume and mute, and the
 * louder of the outputs it reaches that are on */
static int nau_gain(Nau8822State *s, bool right)
{
    int dac = s->regs[right ? R_DAC_VOL_R : R_DAC_VOL_L] & 0xff;
    uint16_t hp = s->regs[right ? R_HP_VOL_R : R_HP_VOL_L];
    uint16_t spk = s->regs[right ? R_SPK_VOL_R : R_SPK_VOL_L];
    bool hp_on = (s->regs[R_PM2] & BIT(right ? 8 : 7)) && !(hp & BIT(6));
    bool spk_on = (s->regs[R_PM3] & BIT(right ? 5 : 6)) && !(spk & BIT(6));
    int out;

    if ((s->regs[R_DAC_CTRL] & BIT(6)) || !dac ||
        !(s->regs[R_PM3] & BIT(right)) || (!hp_on && !spk_on)) {
        return 0;
    }
    out = MAX(hp_on ? hp & 0x3f : 0, spk_on ? spk & 0x3f : 0);
    /* DAC: 0.5 dB steps, 255 = 0 dB; outputs: 1 dB steps, 0x39 = 0 dB */
    return lround(65536.0 * pow(10.0, ((dac - 255) * 0.5 + out - 0x39) / 20));
}

void nau8822_dac(void *opaque, const int16_t *lr, int frames)
{
    Nau8822State *s = opaque;
    int gain[2] = { nau_gain(s, false), nau_gain(s, true) };

    if (!s->voice) {
        return;
    }
    for (int i = 0; i < frames; i++) {
        int at;

        if (s->count == RING) {         /* (no one takes them: the oldest
                                           go) */
            s->head = (s->head + 1) % RING;
            s->count--;
        }
        at = (s->head + s->count++) % RING;
        for (int c = 0; c < 2; c++) {
            int64_t v = ((int64_t)lr[2 * i + c] * gain[c]) >> 16;

            s->ring[2 * at + c] = MIN(MAX(v, INT16_MIN), INT16_MAX);
        }
    }
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->active) {
        s->active = true;
        audio_be_set_active_out(s->audio_be, s->voice, true);
    }
}

/* QEMU's audio has room for @free_b bytes */
static void nau_audio_out(void *opaque, int free_b)
{
    Nau8822State *s = opaque;
    int frames = MIN(free_b / 4, s->count);

    while (frames > 0) {
        int chunk = MIN(frames, RING - s->head);
        int got = audio_be_write(s->audio_be, s->voice,
                                 &s->ring[2 * s->head], chunk * 4) / 4;

        s->head = (s->head + got) % RING;
        s->count -= got;
        frames -= got;
        if (got < chunk) {
            break;
        }
    }
    if (!s->count && s->active &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->last_ns > IDLE_NS) {
        s->active = false;              /* (nothing played for a while) */
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
}

/* ------------------------------------------------------------ the ADC */

/* the left channel's capture gain (1.0: 65536) on the microphone's file,
 * which is what the ADC gives at the defaults: 0 while that path is off */
static int nau_adc_gain(Nau8822State *s)
{
    uint16_t pm2 = s->regs[R_PM2], pga = s->regs[R_PGA_L];
    int adc = s->regs[R_ADC_VOL_L] & 0xff;
    double db;

    /* the left ADC, input PGA and boost stage on; LMICP or LMICN to the
     * PGA; the PGA not muted, the ADC's volume not 0 */
    if ((pm2 & (BIT(0) | BIT(2) | BIT(4))) != (BIT(0) | BIT(2) | BIT(4)) ||
        !(s->regs[R_INPUT] & (BIT(0) | BIT(1))) || (pga & BIT(6)) || !adc) {
        return 0;
    }
    /* PGA: 0.75 dB steps, 0x10 = 0 dB; boost +20 dB; ADC: 0.5 dB steps */
    db = ((pga & 0x3f) - 0x10) * 0.75 +
         (s->regs[R_BOOST_L] & BIT(8) ? 0 : -20) + (adc - 255) * 0.5;
    return lround(65536.0 * pow(10.0, db / 20));
}

void nau8822_adc(void *opaque, int16_t *lr, int frames)
{
    Nau8822State *s = opaque;
    int gain = s->mic && s->fs ? nau_adc_gain(s) : 0;
    double step = s->fs ? (double)s->mic_rate / s->fs : 0;
    bool invert = s->regs[R_ADC_CTRL] & BIT(0);     /* (LADCPL) */

    for (int i = 0; i < frames; i++) {
        int64_t v = 0;

        if (gain) {
            size_t a = (size_t)s->mic_pos;
            double f = s->mic_pos - a;
            double x = s->mic[a] * (1 - f) + s->mic[(a + 1) % s->mic_n] * f;

            v = ((int64_t)lround(invert ? -x : x) * gain) >> 16;
            v = MIN(MAX(v, INT16_MIN), INT16_MAX);
        }
        lr[2 * i] = v;
        lr[2 * i + 1] = 0;              /* (the headset's: none) */
        if (s->mic) {
            s->mic_pos += step;
            if (s->mic_pos >= s->mic_n) {
                s->mic_pos = fmod(s->mic_pos, s->mic_n);    /* (looped) */
            }
        }
    }
}

/* the microphone's file: a RIFF WAVE of 16-bit PCM, its first channel */
static bool nau_mic_load(Nau8822State *s, Error **errp)
{
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize len, p = 12, pcm = 0, pcm_len = 0;
    int fmt = 0, channels = 0, bits = 0;

    if (!g_file_get_contents(s->mic_path, &data, &len, &gerr)) {
        error_setg(errp, "nau8822: the microphone's file: %s", gerr->message);
        return false;
    }
    if (len < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        error_setg(errp, "nau8822: %s is not a WAV file", s->mic_path);
        return false;
    }
    while (p + 8 <= len) {
        uint32_t size = (uint32_t)ldl_le_p(data + p + 4);

        if (!memcmp(data + p, "fmt ", 4) && size >= 16 && p + 24 <= len) {
            fmt = lduw_le_p(data + p + 8);
            channels = lduw_le_p(data + p + 10);
            s->mic_rate = (uint32_t)ldl_le_p(data + p + 12);
            bits = lduw_le_p(data + p + 22);
        } else if (!memcmp(data + p, "data", 4)) {
            pcm = p + 8;                /* (a size not filled in: the rest) */
            pcm_len = size && size <= len - pcm ? size : len - pcm;
            break;
        }
        if (size > len - p - 8) {
            break;
        }
        p += 8 + size + (size & 1);
    }
    if (fmt != 1 || bits != 16 || !channels || !s->mic_rate ||
        pcm_len < 2 * channels) {
        error_setg(errp, "nau8822: %s: the microphone takes 16-bit PCM WAV",
                   s->mic_path);
        return false;
    }
    s->mic_n = pcm_len / (2 * channels);
    s->mic = g_new(int16_t, s->mic_n);
    for (size_t i = 0; i < s->mic_n; i++) {
        s->mic[i] = (int16_t)lduw_le_p(data + pcm + i * 2 * channels);
    }
    return true;
}

/* -------------------------------------------------------------- I2C */

static void nau_reset_regs(Nau8822State *s)
{
    memcpy(s->regs, nau_defaults, sizeof(s->regs));
    nau_clock_update(s);
}

static void nau_write(Nau8822State *s, int reg, uint16_t v)
{
    if (reg == 0) {
        nau_reset_regs(s);              /* (a soft reset: any value) */
        return;
    }
    if (reg >= NAU_REGS || reg == R_REVISION || reg == R_ID) {
        return;
    }
    if (reg == R_PM2 && (v & BIT(0)) && !(s->regs[reg] & BIT(0))) {
        s->mic_pos = 0;                 /* (the left ADC on: a recording) */
    }
    s->regs[reg] = v & 0x1ff;
    switch (reg) {
    case R_PM1:
    case R_CLOCKING:
    case R_PLL_N ... R_PLL_K3:
        nau_clock_update(s);
        break;
    }
}

static int nau_event(I2CSlave *i2c, enum i2c_event event)
{
    Nau8822State *s = NAU8822(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->n = 0;
        break;
    case I2C_START_RECV:
        if (s->n == 1) {
            s->ptr = s->buf[0] >> 1;    /* the register just written */
        }
        s->n = 0;
        s->rd = 0;
        break;
    default:
        break;
    }
    return 0;
}

static int nau_send(I2CSlave *i2c, uint8_t data)
{
    Nau8822State *s = NAU8822(i2c);

    s->buf[s->n++] = data;
    if (s->n == 2) {
        nau_write(s, s->buf[0] >> 1, (s->buf[0] & 1) << 8 | s->buf[1]);
        s->n = 0;
    }
    return 0;
}

static uint8_t nau_recv(I2CSlave *i2c)
{
    Nau8822State *s = NAU8822(i2c);
    uint16_t v = s->ptr < NAU_REGS ? s->regs[s->ptr] : 0;

    return s->rd++ ? v & 0xff : (v >> 8) & 1;
}

void nau8822_link(DeviceState *dev, void (*lrck)(void *opaque, int hz),
                  void *opaque)
{
    Nau8822State *s = NAU8822(dev);

    s->lrck = lrck;
    s->lrck_opaque = opaque;
    lrck(opaque, s->fs);
}

static void nau_reset(DeviceState *dev)
{
    Nau8822State *s = NAU8822(dev);

    s->n = s->rd = s->ptr = 0;
    if (s->voice && s->active) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
    s->active = false;
    s->head = s->count = 0;
    s->mic_pos = 0;
    nau_reset_regs(s);
}

static void nau_realize(DeviceState *dev, Error **errp)
{
    Nau8822State *s = NAU8822(dev);
    Error *err = NULL;

    if (!audio_be_check(&s->audio_be, &err)) {
        warn_report_err(err);           /* (no sound, but it runs) */
        s->audio_be = NULL;
    }
    if (s->mic_path && *s->mic_path) {
        nau_mic_load(s, errp);
    }
}

static const Property nau_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Nau8822State, audio_be),
    DEFINE_PROP_UINT32("mclk", Nau8822State, mclk, 12288000),
    DEFINE_PROP_STRING("mic", Nau8822State, mic_path),
};

static void nau_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    dc->desc = "Nuvoton NAU8822 audio codec";
    dc->realize = nau_realize;
    device_class_set_legacy_reset(dc, nau_reset);
    device_class_set_props(dc, nau_properties);
    k->event = nau_event;
    k->send = nau_send;
    k->recv = nau_recv;
}

static const TypeInfo nau_types[] = {
    {
        .name = TYPE_NAU8822,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Nau8822State),
        .class_init = nau_class_init,
    },
};

DEFINE_TYPES(nau_types)
