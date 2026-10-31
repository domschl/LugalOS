/* user/ceres/src/ceres_sound.c — Chiptune Audio Synthesizer
 *
 * Procedural retro sound effects with zero hardware floating-point operations
 * (pure fixed-point 16.16 and integer math, M-mode mstatus.FS = 0 safe).
 */

#include "ceres_sound.h"
#include <string.h>

#define CH_PULSE_COUNT    2
#define CH_TRI_COUNT      1
#define CH_NOISE_COUNT    1

/* Phase accumulator step: (freq_hz * 65536) / CERES_AUDIO_SAMPLE_RATE */
#define HZ_TO_PHASE_STEP(hz) (((uint32_t)(hz) << 16) / CERES_AUDIO_SAMPLE_RATE)

typedef struct {
    bool     active;
    uint32_t phase;          /* 16.16 phase accumulator */
    uint32_t step;           /* 16.16 phase step per sample */
    int32_t  step_delta;     /* Frequency sweep rate (signed 16.16) */
    int16_t  volume;         /* Current amplitude [0..16384] */
    int16_t  decay;          /* Volume drop per sample */
    uint8_t  duty;           /* 0 = 50%, 1 = 25%, 2 = 12.5% */
    int      samples_left;   /* Duration cutoff */
} voice_pulse_t;

typedef struct {
    bool     active;
    uint32_t phase;
    uint32_t step;
    int32_t  step_delta;
    int16_t  volume;
    int16_t  decay;
    int      samples_left;
} voice_tri_t;

typedef struct {
    bool     active;
    uint32_t lfsr;           /* 16-bit shift register */
    int16_t  volume;
    int16_t  decay;
    int16_t  last_sample;    /* 1-pole low-pass filter state */
    uint8_t  filter_shift;   /* Smoothing coefficient */
    int      samples_left;
} voice_noise_t;

typedef struct {
    bool     active;
    uint16_t notes[8];       /* Frequency in Hz */
    uint8_t  note_count;
    uint8_t  cur_note;
    int      note_duration;  /* Samples per note */
    int      note_timer;     /* Countdown to next note */
} seq_arp_t;

/* Sound state
 * single task: ceres_game */
static bool          s_sound_enabled = true;
/* single task: ceres_game */
static voice_pulse_t s_pulse[CH_PULSE_COUNT];
/* single task: ceres_game */
static voice_tri_t   s_tri;
/* single task: ceres_game */
static voice_noise_t s_noise;
/* single task: ceres_game */
static seq_arp_t     s_arp;

void ceres_sound_init(void) {
    s_sound_enabled = true;
    memset(s_pulse, 0, sizeof(s_pulse));
    memset(&s_tri, 0, sizeof(s_tri));
    memset(&s_noise, 0, sizeof(s_noise));
    memset(&s_arp, 0, sizeof(s_arp));
    s_noise.lfsr = 0xACE1u;
}

void ceres_sound_set_enabled(bool enabled) {
    s_sound_enabled = enabled;
}

bool ceres_sound_is_enabled(void) {
    return s_sound_enabled;
}

void ceres_sound_play(ceres_sfx_id_t sfx) {
    if (!s_sound_enabled) return;

    switch (sfx) {
        case CERES_SFX_LASER: {
            /* High-pitch rapid downward sweep (~1800 Hz down to 350 Hz over 90 ms) */
            voice_pulse_t *v = &s_pulse[0];
            v->active = true;
            v->phase = 0;
            v->step = HZ_TO_PHASE_STEP(1800);
            v->step_delta = -((int32_t)HZ_TO_PHASE_STEP(1450) / (CERES_AUDIO_SAMPLE_RATE * 90 / 1000));
            v->volume = 7000;
            v->decay = 7000 / (CERES_AUDIO_SAMPLE_RATE * 90 / 1000);
            v->duty = 1; /* 25% sharp arcade pulse */
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 90 / 1000;
            break;
        }

        case CERES_SFX_BOMB_DROP: {
            /* Descending smooth whistle (~900 Hz down to 220 Hz over 320 ms) */
            voice_tri_t *v = &s_tri;
            v->active = true;
            v->phase = 0;
            v->step = HZ_TO_PHASE_STEP(900);
            v->step_delta = -((int32_t)HZ_TO_PHASE_STEP(680) / (CERES_AUDIO_SAMPLE_RATE * 320 / 1000));
            v->volume = 8000;
            v->decay = 8000 / (CERES_AUDIO_SAMPLE_RATE * 320 / 1000);
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 320 / 1000;
            break;
        }

        case CERES_SFX_EXPLOSION_TINY: {
            /* Short high-frequency burst (50 ms) */
            voice_noise_t *v = &s_noise;
            v->active = true;
            v->volume = 6000;
            v->decay = 6000 / (CERES_AUDIO_SAMPLE_RATE * 50 / 1000);
            v->filter_shift = 1;
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 50 / 1000;
            break;
        }

        case CERES_SFX_EXPLOSION_MED: {
            /* Crunchy ground target blast with bass rumble (260 ms) */
            voice_noise_t *v = &s_noise;
            v->active = true;
            v->volume = 12000;
            v->decay = 12000 / (CERES_AUDIO_SAMPLE_RATE * 260 / 1000);
            v->filter_shift = 2; /* Heavy low-pass rumble */
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 260 / 1000;

            /* Add sub-bass thump */
            voice_tri_t *vt = &s_tri;
            vt->active = true;
            vt->phase = 0;
            vt->step = HZ_TO_PHASE_STEP(120);
            vt->step_delta = -((int32_t)HZ_TO_PHASE_STEP(80) / (CERES_AUDIO_SAMPLE_RATE * 180 / 1000));
            vt->volume = 9000;
            vt->decay = 9000 / (CERES_AUDIO_SAMPLE_RATE * 180 / 1000);
            vt->samples_left = CERES_AUDIO_SAMPLE_RATE * 180 / 1000;
            break;
        }

        case CERES_SFX_EXPLOSION_HUGE: {
            /* Deep, long seismic reactor blast (650 ms) */
            voice_noise_t *v = &s_noise;
            v->active = true;
            v->volume = 15000;
            v->decay = 15000 / (CERES_AUDIO_SAMPLE_RATE * 650 / 1000);
            v->filter_shift = 3; /* Very heavy low-pass */
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 650 / 1000;

            voice_tri_t *vt = &s_tri;
            vt->active = true;
            vt->phase = 0;
            vt->step = HZ_TO_PHASE_STEP(95);
            vt->step_delta = -((int32_t)HZ_TO_PHASE_STEP(60) / (CERES_AUDIO_SAMPLE_RATE * 400 / 1000));
            vt->volume = 14000;
            vt->decay = 14000 / (CERES_AUDIO_SAMPLE_RATE * 400 / 1000);
            vt->samples_left = CERES_AUDIO_SAMPLE_RATE * 400 / 1000;
            break;
        }

        case CERES_SFX_RESCUE_CHIME: {
            /* Joyful 4-note ascending major arpeggio: C5 (523), E5 (659), G5 (784), C6 (1046) */
            s_arp.active = true;
            s_arp.notes[0] = 523;
            s_arp.notes[1] = 659;
            s_arp.notes[2] = 784;
            s_arp.notes[3] = 1046;
            s_arp.note_count = 4;
            s_arp.cur_note = 0;
            s_arp.note_duration = CERES_AUDIO_SAMPLE_RATE * 55 / 1000;
            s_arp.note_timer = 0;
            break;
        }

        case CERES_SFX_HEROIC_CHIME: {
            /* Grand fanfare: C5 (523), G5 (784), C6 (1046), E6 (1318), G6 (1568) */
            s_arp.active = true;
            s_arp.notes[0] = 523;
            s_arp.notes[1] = 784;
            s_arp.notes[2] = 1046;
            s_arp.notes[3] = 1318;
            s_arp.notes[4] = 1568;
            s_arp.note_count = 5;
            s_arp.cur_note = 0;
            s_arp.note_duration = CERES_AUDIO_SAMPLE_RATE * 65 / 1000;
            s_arp.note_timer = 0;
            break;
        }

        case CERES_SFX_LOW_FUEL: {
            /* Urgent two-tone alert (880 Hz beep) */
            voice_pulse_t *v = &s_pulse[1];
            v->active = true;
            v->phase = 0;
            v->step = HZ_TO_PHASE_STEP(880);
            v->step_delta = 0;
            v->volume = 7500;
            v->decay = 7500 / (CERES_AUDIO_SAMPLE_RATE * 70 / 1000);
            v->duty = 0; /* 50% square */
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 70 / 1000;
            break;
        }

        case CERES_SFX_BARRIER_DOWN: {
            /* Electric arc zap */
            voice_noise_t *vn = &s_noise;
            vn->active = true;
            vn->volume = 9000;
            vn->decay = 9000 / (CERES_AUDIO_SAMPLE_RATE * 140 / 1000);
            vn->filter_shift = 1;
            vn->samples_left = CERES_AUDIO_SAMPLE_RATE * 140 / 1000;

            voice_pulse_t *vp = &s_pulse[0];
            vp->active = true;
            vp->phase = 0;
            vp->step = HZ_TO_PHASE_STEP(240);
            vp->step_delta = HZ_TO_PHASE_STEP(400) / (CERES_AUDIO_SAMPLE_RATE * 140 / 1000);
            vp->volume = 6000;
            vp->decay = 6000 / (CERES_AUDIO_SAMPLE_RATE * 140 / 1000);
            vp->duty = 2;
            vp->samples_left = CERES_AUDIO_SAMPLE_RATE * 140 / 1000;
            break;
        }

        case CERES_SFX_SIREN_PULSE: {
            /* Alternating siren tone */
            voice_pulse_t *v = &s_pulse[1];
            v->active = true;
            v->phase = 0;
            v->step = HZ_TO_PHASE_STEP(750);
            v->step_delta = 0;
            v->volume = 8500;
            v->decay = 8500 / (CERES_AUDIO_SAMPLE_RATE * 120 / 1000);
            v->duty = 0;
            v->samples_left = CERES_AUDIO_SAMPLE_RATE * 120 / 1000;
            break;
        }

        default:
            break;
    }
}

void ceres_sound_render(int16_t *buffer, int num_samples) {
    if (!buffer || num_samples <= 0) return;

    if (!s_sound_enabled) {
        memset(buffer, 0, (size_t)num_samples * sizeof(int16_t));
        return;
    }

    for (int s = 0; s < num_samples; s++) {
        /* 1. Advance Sequenced Arpeggio */
        if (s_arp.active) {
            if (s_arp.note_timer <= 0) {
                if (s_arp.cur_note < s_arp.note_count) {
                    voice_pulse_t *v = &s_pulse[0];
                    v->active = true;
                    v->phase = 0;
                    v->step = HZ_TO_PHASE_STEP(s_arp.notes[s_arp.cur_note]);
                    v->step_delta = 0;
                    v->volume = 8000;
                    v->decay = 8000 / s_arp.note_duration;
                    v->duty = 0;
                    v->samples_left = s_arp.note_duration;

                    s_arp.cur_note++;
                    s_arp.note_timer = s_arp.note_duration;
                } else {
                    s_arp.active = false;
                }
            } else {
                s_arp.note_timer--;
            }
        }

        int32_t mix = 0;

        /* 2. Pulse Wave Channels */
        for (int p = 0; p < CH_PULSE_COUNT; p++) {
            voice_pulse_t *v = &s_pulse[p];
            if (!v->active) continue;

            uint16_t cycle_pos = (uint16_t)(v->phase >> 16);
            uint16_t threshold = 0x8000; /* 50% */
            if (v->duty == 1) threshold = 0x4000; /* 25% */
            else if (v->duty == 2) threshold = 0x2000; /* 12.5% */

            int16_t sample = (cycle_pos < threshold) ? v->volume : -v->volume;
            mix += sample;

            v->phase += v->step;
            v->step = (uint32_t)((int32_t)v->step + v->step_delta);
            v->volume -= v->decay;
            if (v->volume < 0) v->volume = 0;

            v->samples_left--;
            if (v->samples_left <= 0 || v->volume <= 0) {
                v->active = false;
            }
        }

        /* 3. Triangle Wave Channel */
        if (s_tri.active) {
            uint16_t pos = (uint16_t)(s_tri.phase >> 16);
            int16_t sample;
            if (pos < 0x8000) {
                /* Rising slope -vol .. +vol */
                sample = (int16_t)(-s_tri.volume + (int32_t)(pos) * (2 * s_tri.volume) / 0x8000);
            } else {
                /* Falling slope +vol .. -vol */
                sample = (int16_t)(s_tri.volume - (int32_t)(pos - 0x8000) * (2 * s_tri.volume) / 0x8000);
            }
            mix += sample;

            s_tri.phase += s_tri.step;
            s_tri.step = (uint32_t)((int32_t)s_tri.step + s_tri.step_delta);
            s_tri.volume -= s_tri.decay;
            if (s_tri.volume < 0) s_tri.volume = 0;

            s_tri.samples_left--;
            if (s_tri.samples_left <= 0 || s_tri.volume <= 0) {
                s_tri.active = false;
            }
        }

        /* 4. Noise Channel (16-bit Galois LFSR) */
        if (s_noise.active) {
            /* LFSR algorithm: taps 16, 14, 13, 11 */
            uint32_t bit = ((s_noise.lfsr >> 0) ^ (s_noise.lfsr >> 2) ^ (s_noise.lfsr >> 3) ^ (s_noise.lfsr >> 5)) & 1u;
            s_noise.lfsr = (s_noise.lfsr >> 1) | (bit << 15);

            int32_t raw = (int32_t)(s_noise.lfsr & 0xFFFF) - 32768;
            int16_t scaled = (int16_t)((raw * s_noise.volume) >> 15);

            /* Simple integer low-pass filter: y[n] = y[n-1] + (x[n] - y[n-1]) >> shift */
            s_noise.last_sample += (scaled - s_noise.last_sample) >> s_noise.filter_shift;
            mix += s_noise.last_sample;

            s_noise.volume -= s_noise.decay;
            if (s_noise.volume < 0) s_noise.volume = 0;

            s_noise.samples_left--;
            if (s_noise.samples_left <= 0 || s_noise.volume <= 0) {
                s_noise.active = false;
            }
        }

        /* Saturation Limiter [-32768, 32767] */
        if (mix > 32767) mix = 32767;
        else if (mix < -32768) mix = -32768;

        buffer[s] = (int16_t)mix;
    }
}
