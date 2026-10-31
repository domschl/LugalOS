/* drivers/audiotest_esp32p4.c — Audio Hardware Diagnostic Tool for ESP32-P4
 *
 * Implements hardware verification for audio codecs (ES8311, ES8388), speaker
 * power amplifiers (NS4150B via PI4IOE1 on Tab5 or GPIO 53 on LCD-7B/NANO),
 * and the ESP32-P4 I2S0 peripheral with AHB-GDMA descriptor audio streaming.
 *
 * Concurrency: Stateless shell command execution; delegates audio streaming and
 * codec management to drivers/audio_esp32p4.c under the shell's single-task
 * execution domain.
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "drivers/audiotest_esp32p4.h"
#include "drivers/audio_esp32p4.h"
#include "lugalos_config.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "drivers/i2c_bus.h"

#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* Diagnostic I2S0 and HP_SYS_CLKRST registers */
#define P4_I2S0_BASE                    0x500C6000UL
#define I2S0_TX_CONF                    (P4_I2S0_BASE + 0x0024UL)
#define I2S0_TX_CONF1                   (P4_I2S0_BASE + 0x002CUL)
#define I2S0_STATE                      (P4_I2S0_BASE + 0x006CUL)
#define I2S0_DATE                       (P4_I2S0_BASE + 0x0080UL)

#define P4_HP_SYS_CLKRST_BASE           0x500E6000UL
#define P4_CLKRST_SOC_CLK_CTRL2         (P4_HP_SYS_CLKRST_BASE + 0x001CUL)
#define P4_CLKRST_HP_RST_EN2            (P4_HP_SYS_CLKRST_BASE + 0x00C8UL)

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* --- Pure Fixed-Point Tone Synthesizer --- */

typedef struct {
    uint32_t phase;
    uint32_t phase_inc;
    uint32_t total;
    uint32_t played;
} tone_synth_t;

static void tone_synth_cb(int16_t *buf, uint32_t samples, void *user) {
    tone_synth_t *t = (tone_synth_t *)user;
    int16_t amp = (int16_t)((AUDIO_P4_DEFAULT_PEAK * 3) / 4);

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t cur = t->played + i;
        int32_t cur_amp = amp;

        /* 10ms attack (160 samples), 20ms release (320 samples) */
        if (cur < 160) {
            cur_amp = (int32_t)amp * (int32_t)cur / 160;
        } else if (cur + 320 >= t->total) {
            uint32_t rem = t->total - cur;
            cur_amp = (int32_t)amp * (int32_t)rem / 320;
        }

        int16_t s = (int16_t)(((int32_t)audio_p4_sine((uint16_t)(t->phase >> 16)) * cur_amp) >> 15);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
        t->phase += t->phase_inc;
    }
    t->played += samples;
}

static void p4_audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    uint32_t total_samples = (16000u * duration_ms) / 1000u;
    tone_synth_t t = {
        .phase     = 0,
        .phase_inc = (uint32_t)(((uint64_t)freq_hz * 4294967296ULL) / 16000u),
        .total     = total_samples,
        .played    = 0,
    };
    audio_p4_play_stream(tone_synth_cb, &t, total_samples);
}

/* --- Pure Fixed-Point Celestial Chord Synthesizer --- */

typedef struct {
    uint32_t phase_a, phase_b, phase_c, phase_d;
    uint32_t inc_a, inc_b, inc_c, inc_d;
    uint32_t env;
    uint32_t sample_idx;
    uint32_t total_samples;
} chord_synth_t;

static void chord_synth_cb(int16_t *buf, uint32_t samples, void *user) {
    chord_synth_t *cs = (chord_synth_t *)user;
    const int32_t target_peak = AUDIO_P4_CHORD_PEAK;

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t cur = cs->sample_idx + i;

        /* Exponential decay: env drops by ~1/65536 each sample (~2.8s half-life) */
        cs->env -= (cs->env >> 16);
        int32_t cur_env = (int32_t)(cs->env >> 16); /* 0 .. 32767 */

        /* 15ms smooth attack (240 samples @ 16kHz) to eliminate sudden DC transient */
        if (cur < 240) {
            cur_env = (cur_env * (int32_t)cur) / 240;
        } else if (cur + 8000 >= cs->total_samples) {
            /* Smooth 500ms fadeout to silence at the end of the chord */
            uint32_t rem = (cur < cs->total_samples) ? (cs->total_samples - cur) : 0;
            cur_env = (cur_env * (int32_t)rem) / 8000;
        }

        /* Pure fixed-point 32-bit harmonic accumulation: 4 consonant voices summing to 32768 */
        int32_t s_a = audio_p4_sine((uint16_t)(cs->phase_a >> 16));
        int32_t s_b = audio_p4_sine((uint16_t)(cs->phase_b >> 16));
        int32_t s_c = audio_p4_sine((uint16_t)(cs->phase_c >> 16));
        int32_t s_d = audio_p4_sine((uint16_t)(cs->phase_d >> 16));

        int32_t mixed = (s_a * 8192 + s_b * 8192 + s_c * 8192 + s_d * 8192) >> 15;
        int32_t out32 = (((mixed * target_peak) >> 15) * cur_env) >> 15;

        if (out32 > 32767) out32 = 32767;
        if (out32 < -32768) out32 = -32768;

        int16_t out = (int16_t)out32;
        buf[i * 2]     = out;
        buf[i * 2 + 1] = out;

        cs->phase_a += cs->inc_a;
        cs->phase_b += cs->inc_b;
        cs->phase_c += cs->inc_c;
        cs->phase_d += cs->inc_d;
    }
    cs->sample_idx += samples;
}

static void p4_audio_play_chord(void) {
    /* Pure harmonic celestial chime: A3 (220 Hz), E4 (330 Hz), A4 (440 Hz), E5 (660 Hz)
     * All integer harmonics of 110 Hz (2x, 3x, 4x, 6x) for completely clean, beat-free resonance */
    uint32_t total_samples = 16000u * 3; /* 3.0 seconds */
    chord_synth_t cs = {
        .phase_a       = 0,
        .phase_b       = 0,
        .phase_c       = 0,
        .phase_d       = 0,
        .inc_a         = (uint32_t)(((uint64_t)220u * 4294967296ULL) / 16000u),
        .inc_b         = (uint32_t)(((uint64_t)330u * 4294967296ULL) / 16000u),
        .inc_c         = (uint32_t)(((uint64_t)440u * 4294967296ULL) / 16000u),
        .inc_d         = (uint32_t)(((uint64_t)660u * 4294967296ULL) / 16000u),
        .env           = 0x7FFF0000u,
        .sample_idx    = 0,
        .total_samples = total_samples,
    };
    audio_p4_play_stream(chord_synth_cb, &cs, total_samples);
}

/* --- Pure Fixed-Point Melody Synthesizer (Beethoven Für Elise) --- */

typedef struct {
    uint16_t freq;
    uint16_t dur_ms;
} melody_note_t;

static const melody_note_t s_fur_elise[] = {
    { 659, 220 }, { 622, 220 }, { 659, 220 }, { 622, 220 }, { 659, 220 },
    { 494, 220 }, { 587, 220 }, { 523, 220 }, { 440, 480 },
    { 262, 220 }, { 330, 220 }, { 440, 220 }, { 494, 480 },
    { 330, 220 }, { 415, 220 }, { 494, 220 }, { 523, 480 },
    { 330, 220 }, { 659, 220 }, { 622, 220 }, { 659, 220 }, { 622, 220 },
    { 659, 220 }, { 494, 220 }, { 587, 220 }, { 523, 220 }, { 440, 700 },
};

typedef struct {
    size_t note_idx;
    uint32_t note_sample;
    uint32_t note_total_samples;
    uint32_t phase;
    uint32_t phase_inc;
} melody_synth_t;

static void melody_synth_cb(int16_t *buf, uint32_t samples, void *user) {
    melody_synth_t *ms = (melody_synth_t *)user;
    size_t total_notes = sizeof(s_fur_elise) / sizeof(s_fur_elise[0]);
    int16_t base_amp = (int16_t)((AUDIO_P4_DEFAULT_PEAK * 3) / 4);

    for (uint32_t i = 0; i < samples; i++) {
        if (ms->note_idx >= total_notes) {
            buf[i * 2]     = 0;
            buf[i * 2 + 1] = 0;
            continue;
        }

        if (ms->note_sample >= ms->note_total_samples) {
            ms->note_idx++;
            ms->note_sample = 0;
            if (ms->note_idx < total_notes) {
                uint32_t freq = s_fur_elise[ms->note_idx].freq;
                ms->phase_inc = (uint32_t)(((uint64_t)freq * 4294967296ULL) / 16000u);
                ms->note_total_samples = (16000u * s_fur_elise[ms->note_idx].dur_ms) / 1000u;
            }
        }

        if (ms->note_idx >= total_notes) {
            buf[i * 2]     = 0;
            buf[i * 2 + 1] = 0;
            continue;
        }

        /* 8ms attack (128 samples), 16ms release (256 samples) for note articulation */
        int32_t cur_amp = base_amp;
        if (ms->note_sample < 128) {
            cur_amp = (int32_t)base_amp * (int32_t)ms->note_sample / 128;
        } else if (ms->note_sample + 256 >= ms->note_total_samples) {
            uint32_t rem = ms->note_total_samples - ms->note_sample;
            cur_amp = (int32_t)base_amp * (int32_t)rem / 256;
        }

        int16_t out = (int16_t)(((int32_t)audio_p4_sine((uint16_t)(ms->phase >> 16)) * cur_amp) >> 15);
        buf[i * 2]     = out;
        buf[i * 2 + 1] = out;

        ms->phase += ms->phase_inc;
        ms->note_sample++;
    }
}

static void p4_audio_play_melody(void) {
    size_t total_notes = sizeof(s_fur_elise) / sizeof(s_fur_elise[0]);
    uint32_t total_ms = 0;
    for (size_t i = 0; i < total_notes; i++) {
        total_ms += s_fur_elise[i].dur_ms;
    }
    uint32_t total_samples = (16000u * total_ms) / 1000u;

    melody_synth_t ms = {
        .note_idx = 0,
        .note_sample = 0,
        .note_total_samples = (16000u * s_fur_elise[0].dur_ms) / 1000u,
        .phase = 0,
        .phase_inc = (uint32_t)(((uint64_t)s_fur_elise[0].freq * 4294967296ULL) / 16000u),
    };

    audio_p4_play_stream(melody_synth_cb, &ms, total_samples);
}

/* --- Public Shell Entry Point --- */

static bool parse_u32(const char **str, uint32_t *out) {
    const char *p = *str;
    while (*p == ' ') p++;
    if (!*p) return false;
    uint32_t val = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (!*p) return false;
        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            val <<= 4;
            if (*p >= '0' && *p <= '9') val |= (uint32_t)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') val |= (uint32_t)(*p - 'a' + 10);
            else val |= (uint32_t)(*p - 'A' + 10);
            p++;
        }
    } else {
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            p++;
        }
    }
    *str = p;
    *out = val;
    return true;
}

void audiotest_cmd(const char *arg) {
    while (arg && *arg == ' ') arg++;

    if (arg && strcmp(arg, "help") == 0) {
        cprintf("Usage: audiotest [status | init | amp on | amp off | dump | reg <r> [v] | exp | tone [hz] [ms] | chord | melody]\n");
        cprintf("  status            - Probe codec, report amplifier & I2S0 peripheral state (default)\n");
        cprintf("  init              - Initialize codec DAC path and enable speaker power amplifier\n");
        cprintf("  amp on            - Turn on NS4150B speaker amplifier\n");
        cprintf("  amp off           - Turn off NS4150B speaker amplifier\n");
        cprintf("  dump              - Dump all audio codec I2C registers\n");
        cprintf("  reg <r> [v]       - Read codec register <r> or write value [v]\n");
#if defined(CONFIG_BOARD_TAB5)
        cprintf("  exp               - Dump PI4IOE1 (0x43) and PI4IOE2 (0x44) IO expander registers\n");
#endif
        cprintf("  tone [hz] [ms]    - Play pure fixed-point sine test tone (default 440 Hz, 500 ms)\n");
        cprintf("  chord             - Play celestial 4-voice space ambient chord\n");
        cprintf("  melody            - Play opening theme of Beethoven Fuer Elise\n");
        return;
    }

    if (arg && strcmp(arg, "amp on") == 0) {
        if (audio_p4_amp_set(true)) {
            cprintf("audiotest: speaker power amplifier ENABLED.\n");
        } else {
            cprintf("audiotest: failed to enable speaker power amplifier.\n");
        }
        return;
    }

    if (arg && strcmp(arg, "amp off") == 0) {
        if (audio_p4_amp_set(false)) {
            cprintf("audiotest: speaker power amplifier MUTED (disabled).\n");
        } else {
            cprintf("audiotest: failed to mute speaker power amplifier.\n");
        }
        return;
    }

    if (arg && strcmp(arg, "dump") == 0) {
        audio_p4_codec_dump();
        return;
    }

    if (arg && (strncmp(arg, "vol", 3) == 0 || strncmp(arg, "volume", 6) == 0)) {
        const char *p = arg + ((arg[3] == 'u' || arg[3] == 'U') ? 6 : 3);
        uint32_t v = 0;
        if (parse_u32(&p, &v)) {
            if (v > 100) v = 100;
            audio_p4_set_volume((int)v);
            cprintf("audiotest: volume set to %u%%\n", (unsigned)v);
        } else {
            cprintf("audiotest: current volume is %d%%\n", audio_p4_get_volume());
        }
        return;
    }

    if (arg && strncmp(arg, "reg", 3) == 0) {
        const char *p = arg + 3;
        uint32_t reg = 0, val = 0;
        if (!parse_u32(&p, &reg)) {
            cprintf("Usage: audiotest reg <register> [value]\n");
            return;
        }
        if (parse_u32(&p, &val)) {
            if (audio_p4_codec_write((uint8_t)reg, (uint8_t)val)) {
                cprintf("audiotest: wrote %s [0x%02x] = 0x%02x\n", audio_p4_codec_name(), (unsigned)reg, (unsigned)val);
            } else {
                cprintf("audiotest: write failed for %s [0x%02x]\n", audio_p4_codec_name(), (unsigned)reg);
            }
        } else {
            uint8_t rval = 0;
            if (audio_p4_codec_read((uint8_t)reg, &rval)) {
                cprintf("%s [0x%02x] = 0x%02x\n", audio_p4_codec_name(), (unsigned)reg, rval);
            } else {
                cprintf("%s [0x%02x] = <NACK>\n", audio_p4_codec_name(), (unsigned)reg);
            }
        }
        return;
    }

#if defined(CONFIG_BOARD_TAB5)
    if (arg && strcmp(arg, "exp") == 0) {
        cprintf("PI4IOE1 (0x43) IO Expander Registers:\n");
        cprintf("  [0x03] IO_DIR    = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x03u));
        cprintf("  [0x05] OUT_SET   = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x05u));
        cprintf("  [0x07] OUT_H_IM  = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x07u));
        cprintf("  [0x0B] PULL_EN   = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x0Bu));
        cprintf("  [0x0D] PULL_SEL  = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x0Du));
        cprintf("  [0x0F] IN_STA    = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x43u, 0x0Fu));

        cprintf("PI4IOE2 (0x44) IO Expander Registers:\n");
        cprintf("  [0x03] IO_DIR    = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x03u));
        cprintf("  [0x05] OUT_SET   = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x05u));
        cprintf("  [0x07] OUT_H_IM  = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x07u));
        cprintf("  [0x0B] PULL_EN   = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x0Bu));
        cprintf("  [0x0D] PULL_SEL  = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x0Du));
        cprintf("  [0x0F] IN_STA    = 0x%02x\n", (unsigned)(uint8_t)i2c_tab5_expander_read(0x44u, 0x0Fu));
        return;
    }
#endif

    if (arg && strcmp(arg, "init") == 0) {
        cprintf("audiotest: initializing %s codec...\n", audio_p4_codec_name());
        if (audio_p4_init()) {
            audio_p4_amp_set(true);
            cprintf("[PASS] %s codec initialized for DAC output; speaker amplifier ENABLED.\n", audio_p4_codec_name());
        } else {
            cprintf("[FAIL] Failed to initialize %s codec over I2C.\n", audio_p4_codec_name());
        }
        return;
    }

    if (arg && strncmp(arg, "tone", 4) == 0) {
        const char *p = arg + 4;
        while (*p == ' ') p++;
        uint32_t freq = 440;
        uint32_t duration_ms = 500;
        if (*p >= '0' && *p <= '9') {
            freq = 0;
            while (*p >= '0' && *p <= '9') freq = freq * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                duration_ms = 0;
                while (*p >= '0' && *p <= '9') duration_ms = duration_ms * 10 + (*p++ - '0');
            }
        }
        if (freq < 20 || freq > 8000) freq = 440;
        if (duration_ms < 50 || duration_ms > 10000) duration_ms = 500;
        cprintf("audiotest: playing %u Hz test tone for %u ms...\n", (unsigned)freq, (unsigned)duration_ms);
        p4_audio_play_tone(freq, duration_ms);
        cprintf("audiotest: done.\n");
        return;
    }

    if (arg && strcmp(arg, "chord") == 0) {
        cprintf("audiotest: playing celestial space ambient chord...\n");
        p4_audio_play_chord();
        cprintf("audiotest: done.\n");
        return;
    }

    if (arg && strcmp(arg, "melody") == 0) {
        cprintf("audiotest: playing Fuer Elise melody...\n");
        p4_audio_play_melody();
        cprintf("audiotest: done.\n");
        return;
    }

    /* Default: audiotest status */
    cprintf("=== ESP32-P4 Audio Hardware Diagnostics ===\n");
#if defined(CONFIG_BOARD_DESC)
    cprintf("Board Target:      %s\n", CONFIG_BOARD_DESC);
#endif
#if defined(CONFIG_BOARD_TAB5)
    cprintf("Audio Codec:       ES8388 at 0x10 on SYS_I2C (GPIO 31/32)\n");
    cprintf("Power Amplifier:   NS4150B controlled by PI4IOE1 (0x43) Pin 1 (SPK_EN)\n");
    cprintf("I2S0 Pin Routing:  MCLK=GPIO30, BCLK=GPIO27, WS=GPIO29, DOUT=GPIO26\n");
#else
    cprintf("Audio Codec:       ES8311 at 0x18 on I2C0 (GPIO 7/8)\n");
    cprintf("Power Amplifier:   NS4150B controlled by GPIO 53\n");
    cprintf("I2S0 Pin Routing:  MCLK=GPIO13, BCLK=GPIO12, WS=GPIO10, DOUT=GPIO9\n");
#endif

    /* 1. Probe Codec on I2C */
    cprintf("\n--- 1. Codec I2C Probe ---\n");
#if defined(CONFIG_BOARD_TAB5)
    uint8_t ctl0 = 0, ctl1 = 0, ctl2 = 0, dacp = 0;
    bool ok0 = audio_p4_codec_read(0x00, &ctl0);
    bool ok1 = audio_p4_codec_read(0x01, &ctl1);
    bool ok2 = audio_p4_codec_read(0x02, &ctl2);
    bool ok4 = audio_p4_codec_read(0x04, &dacp);
    if (ok0 && ok1 && ok2 && ok4) {
        cprintf("  [PASS] ES8388 responded at 0x10\n");
        cprintf("  Control0=0x%02x Control1=0x%02x ChipPower=0x%02x DACPower=0x%02x\n",
                ctl0, ctl1, ctl2, dacp);
    } else {
        cprintf("  [FAIL] ES8388 at 0x10 did not answer I2C read!\n");
    }
#else
    uint8_t id1 = 0, id2 = 0, rev = 0, ctl0 = 0;
    bool ok1 = audio_p4_codec_read(0xFD, &id1);
    bool ok2 = audio_p4_codec_read(0xFE, &id2);
    bool ok3 = audio_p4_codec_read(0xFF, &rev);
    bool ok0 = audio_p4_codec_read(0x00, &ctl0);
    if (ok1 && ok2 && ok3 && ok0 && id1 == 0x83 && id2 == 0x11) {
        cprintf("  [PASS] ES8311 responded at 0x18 (Silicon Signature: 0x%02x%02x, Rev: 0x%02x)\n",
                id1, id2, rev);
        cprintf("  Reset/Power Register [0x00] = 0x%02x\n", ctl0);
    } else {
        cprintf("  [FAIL] ES8311 at 0x18 did not match chip ID (read: 0x%02x 0x%02x)!\n", id1, id2);
    }
#endif

    /* 2. Speaker Power Amplifier */
    cprintf("\n--- 2. Speaker Power Amplifier ---\n");
    int amp_state = audio_p4_amp_get();
    if (amp_state >= 0) {
        cprintf("  Speaker Amplifier (NS4150B): %s\n",
                amp_state ? "ENABLED (active)" : "MUTED (standby/low-power)");
#if defined(CONFIG_BOARD_TAB5)
        int out_set = i2c_tab5_expander_read(0x43u, 0x05u);
        if (out_set >= 0) {
            cprintf("  PI4IOE1 (0x43) Pins:         SPK_EN=%d, EXT5V_EN=%d (OUT_SET=0x%02x)\n",
                    (out_set & 0x02) ? 1 : 0, (out_set & 0x04) ? 1 : 0, (unsigned)(uint8_t)out_set);
        }
#endif
    } else {
        cprintf("  [FAIL] Unable to read amplifier control status.\n");
    }

    /* 3. I2S0 Subsystem Clock & Registers */
    cprintf("\n--- 3. ESP32-P4 I2S0 Peripheral Subsystem ---\n");
    uint32_t clk_ctrl2 = REG32(P4_CLKRST_SOC_CLK_CTRL2);
    uint32_t rst_en2   = REG32(P4_CLKRST_HP_RST_EN2);
    bool clk_on = (clk_ctrl2 & (1u << 14)) != 0;
    bool in_rst = (rst_en2 & (1u << 4)) != 0;

    cprintf("  I2S0 APB Clock Gate: %s (HP_SYS_CLKRST bit 14 = %d)\n",
            clk_on ? "ENABLED" : "GATED OFF", clk_on ? 1 : 0);
    cprintf("  I2S0 Reset Status:   %s (HP_SYS_CLKRST bit 4 = %d)\n",
            in_rst ? "ASSERTED (in reset)" : "RELEASED (running)", in_rst ? 1 : 0);

    if (!clk_on || in_rst) {
        cprintf("  (Tip: run 'audiotest init' or 'audiotest tone' to start audio output)\n");
    } else {
        uint32_t i2s_date  = REG32(I2S0_DATE);
        uint32_t i2s_state = REG32(I2S0_STATE);
        uint32_t i2s_conf1 = REG32(I2S0_TX_CONF1);
        uint32_t i2s_tx    = REG32(I2S0_TX_CONF);
        cprintf("  I2S0 HW Registers:   DATE=0x%08x STATE=0x%08x CONF1=0x%08x TX_CONF=0x%08x\n",
                i2s_date, i2s_state, i2s_conf1, i2s_tx);
    }
}

void volume_cmd(const char *arg) {
    if (!arg) {
        cprintf("Volume: %d%%\n", audio_p4_get_volume());
        return;
    }
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        cprintf("Volume: %d%%\n", audio_p4_get_volume());
        return;
    }
    uint32_t val = 0;
    const char *p = arg;
    if (parse_u32(&p, &val)) {
        if (val > 100) val = 100;
        audio_p4_set_volume((int)val);
        cprintf("Volume set to %u%%\n", (unsigned)val);
    } else {
        cprintf("Usage: volume [0-100]\n");
    }
}

#else /* Non-ESP32-P4 targets */

void audiotest_cmd(const char *arg) {
    (void)arg;
    cprintf("audiotest: requires ESP32-P4 target with audio hardware.\n");
}

void volume_cmd(const char *arg) {
    (void)arg;
    cprintf("volume: requires hardware audio target.\n");
}

#endif
