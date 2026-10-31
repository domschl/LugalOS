/* user/ceres/src/ceres_ambient.c — Generative Space Ambient Synthesizer
 *
 * Minimal, meditative, procedural ambient music generator with zero floating-point
 * operations (pure 16.16 fixed-point and integer math, M-mode mstatus.FS = 0 safe).
 *
 * Layers:
 * 1. Deep Sub-Bass Root Drone: Detuned dual oscillator producing slow acoustic beating chorus.
 * 2. Harmonic Pad: Slow LFO volume breathing on consonant chord tones.
 * 3. Generative Crystalline Starlight Chimes: Sparse modal bell tones with soft attack and inharmonic ring.
 * 4. Celestial Solar Wind: Whispering low-passed pink noise with slow sweeping cutoff LFO.
 */

#include "ceres_ambient.h"
#include "ceres_cyber.h"
#include <string.h>

#define CHIME_VOICES 3

/* 256-entry sine table mapped to [-32767, 32767] */
static const int16_t s_sin_tab[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

/* Linear interpolated sine query using 16-bit phase */
static inline int16_t ambient_sine(uint16_t phase) {
    uint8_t idx = (uint8_t)(phase >> 8);
    uint8_t frac = (uint8_t)(phase & 0xFF);
    int32_t v0 = s_sin_tab[idx];
    int32_t v1 = s_sin_tab[(uint8_t)(idx + 1)];
    return (int16_t)(v0 + (((v1 - v0) * frac) >> 8));
}

/* Macro for tenth-of-a-hertz frequency step (e.g. 73.4 Hz -> 734) */
#define AMBIENT_HZ10_TO_STEP(hz10) (((uint32_t)(hz10) * 65536u) / ((uint32_t)CERES_AMBIENT_SAMPLE_RATE * 10u))

/* Chime Envelope States */
typedef enum {
    CHIME_STATE_IDLE = 0,
    CHIME_STATE_ATTACK,
    CHIME_STATE_DECAY
} chime_state_t;

typedef struct {
    bool          active;
    chime_state_t state;
    uint32_t      phase_fund;
    uint32_t      step_fund;
    uint32_t      phase_over;
    uint32_t      step_over;
    int32_t       env;          /* 16.16 fixed-point envelope [0..65536] */
    int32_t       env_attack;   /* Increment per sample during attack */
    int32_t       volume;       /* Base peak volume [0..32767] */
} chime_voice_t;

/* Ambient Synth State
 * single task: ceres_game */
static bool                 s_ambient_enabled = true;
/* single task: ceres_game */
static int                  s_master_vol = CERES_AMBIENT_DEFAULT_VOLUME;   /* [0..256] */
/* single task: ceres_game */
static ceres_ambient_mood_t s_mood = CERES_AMBIENT_MOOD_DEEP_SPACE;
/* single task: ceres_game */
static int                  s_duck_level = 0;     /* [0..256] */

/* West Coast Buchla Wavefolder State
 * single task: ceres_game */
static int                  s_target_fold_drive = 256;  /* [256..1024], default unity bypass */
/* single task: ceres_game */
static int                  s_current_fold_drive = 256;
/* single task: ceres_game */
static int32_t              s_fold_filter_drone = 0;
/* single task: ceres_game */
static int32_t              s_fold_filter_pad = 0;

/* Drone Layer */
/* single task: ceres_game */
static uint32_t             s_drone_phase_a = 0;
/* single task: ceres_game */
static uint32_t             s_drone_step_a = 0;
/* single task: ceres_game */
static uint32_t             s_drone_phase_b = 0;
/* single task: ceres_game */
static uint32_t             s_drone_step_b = 0;
/* single task: ceres_game */
static uint32_t             s_drone_phase_oct = 0;
/* single task: ceres_game */
static uint32_t             s_drone_step_oct = 0;
/* single task: ceres_game */
static uint32_t             s_drone_phase_fifth = 0;
/* single task: ceres_game */
static uint32_t             s_drone_step_fifth = 0;

/* Harmonic Pad Layer */
/* single task: ceres_game */
static uint32_t             s_pad_phase_a = 0;
/* single task: ceres_game */
static uint32_t             s_pad_step_a = 0;
/* single task: ceres_game */
static uint32_t             s_pad_phase_b = 0;
/* single task: ceres_game */
static uint32_t             s_pad_step_b = 0;
/* single task: ceres_game */
static uint32_t             s_pad_lfo_phase = 0;
/* single task: ceres_game */
static uint32_t             s_pad_lfo_step = 0;

/* Celestial Wind Layer */
/* single task: ceres_game */
static uint32_t             s_wind_lfsr = 0x55AAu;
/* single task: ceres_game */
static int32_t              s_wind_lp = 0;
/* single task: ceres_game */
static uint32_t             s_wind_lfo_phase = 0;
/* single task: ceres_game */
static uint32_t             s_wind_lfo_step = 0;

/* Crystalline Chimes Layer */
/* single task: ceres_game */
static chime_voice_t        s_chimes[CHIME_VOICES];
/* single task: ceres_game */
static int32_t              s_chime_timer = 0;
/* single task: ceres_game */
static uint32_t             s_seq_rng = 0x9B41u;

/* Modal Scale Pitch Pools in Tenths of Hz */
static const uint16_t s_scale_deep_space[12] = {
    1468, 1746, 2200, 2616, 2937, 3296, 3492, 3920, 4400, 5233, 5873, 6593
}; /* D3, F3, A3, C4, D4, E4, F4, G4, A4, C5, D5, E5 (D Dorian) */

static const uint16_t s_scale_cavern[10] = {
    1308, 1556, 1960, 2331, 2616, 2937, 3111, 3920, 4662, 5233
}; /* C3, Eb3, G3, Bb3, C4, D4, Eb4, G4, Bb4, C5 (C Minor) */

static const uint16_t s_scale_tense[10] = {
    1648, 1960, 2469, 2616, 3296, 3492, 3920, 4939, 5233, 6593
}; /* E3, G3, B3, C4, E4, F4, G4, B4, C5, E5 (E Minor / Phrygian) */

static uint32_t ambient_rand(void) {
    /* 16-bit Galois LFSR */
    uint32_t bit = ((s_seq_rng >> 0) ^ (s_seq_rng >> 2) ^ (s_seq_rng >> 3) ^ (s_seq_rng >> 5)) & 1u;
    s_seq_rng = (s_seq_rng >> 1) | (bit << 15);
    return s_seq_rng;
}

static void update_mood_tuning(void) {
    uint32_t root_hz10;
    uint32_t third_hz10;
    uint32_t fifth_hz10;

    switch (s_mood) {
        case CERES_AMBIENT_MOOD_CAVERN:
            root_hz10 = 654;    /* C2 = 65.4 Hz */
            third_hz10 = 1556;  /* Eb3 = 155.6 Hz */
            fifth_hz10 = 980;   /* G2 = 98.0 Hz */
            break;
        case CERES_AMBIENT_MOOD_TENSE:
            root_hz10 = 824;    /* E2 = 82.4 Hz */
            third_hz10 = 1960;  /* G3 = 196.0 Hz */
            fifth_hz10 = 1235;  /* B2 = 123.5 Hz */
            break;
        case CERES_AMBIENT_MOOD_DEEP_SPACE:
        default:
            root_hz10 = 734;    /* D2 = 73.4 Hz */
            third_hz10 = 1746;  /* F3 = 174.6 Hz */
            fifth_hz10 = 1100;  /* A2 = 110.0 Hz */
            break;
    }

    /* Detuned dual oscillators (0.8 Hz acoustic beat) */
    s_drone_step_a = AMBIENT_HZ10_TO_STEP(root_hz10 - 4);
    s_drone_step_b = AMBIENT_HZ10_TO_STEP(root_hz10 + 4);
    /* Psychoacoustic upper harmonics (octave and fifth) */
    s_drone_step_oct = AMBIENT_HZ10_TO_STEP(root_hz10 * 2);
    s_drone_step_fifth = AMBIENT_HZ10_TO_STEP(fifth_hz10 * 2);

    /* Harmonic pad 2-voice chord */
    s_pad_step_a = AMBIENT_HZ10_TO_STEP(fifth_hz10);
    s_pad_step_b = AMBIENT_HZ10_TO_STEP(third_hz10);
}

void ceres_ambient_init(void) {
    s_ambient_enabled = true;
    s_master_vol = CERES_AMBIENT_DEFAULT_VOLUME;
    s_mood = CERES_AMBIENT_MOOD_DEEP_SPACE;
    s_duck_level = 0;
    s_target_fold_drive = 256;
    s_current_fold_drive = 256;
    s_fold_filter_drone = 0;
    s_fold_filter_pad = 0;

    ceres_cyber_init();

    s_drone_phase_a = 0;
    s_drone_phase_b = 0;
    s_drone_phase_oct = 0;
    s_drone_phase_fifth = 0;
    s_pad_phase_a = 0;
    s_pad_phase_b = 0;
    s_pad_lfo_phase = 0;
    /* Slow ~13.5 sec pad breathing LFO (0.074 Hz) */
    s_pad_lfo_step = (uint32_t)(((uint64_t)65536u * 74u) / ((uint64_t)CERES_AMBIENT_SAMPLE_RATE * 1000u));

    s_wind_lfsr = 0x55AAu;
    s_wind_lp = 0;
    s_wind_lfo_phase = 0;
    /* Very slow ~26 sec solar wind sweep LFO (0.038 Hz) */
    s_wind_lfo_step = (uint32_t)(((uint64_t)65536u * 38u) / ((uint64_t)CERES_AMBIENT_SAMPLE_RATE * 1000u));

    memset(s_chimes, 0, sizeof(s_chimes));
    s_chime_timer = CERES_AMBIENT_SAMPLE_RATE * 2; /* First chime in 2 seconds */
    s_seq_rng = 0x9B41u;

    update_mood_tuning();
}

void ceres_ambient_set_enabled(bool enabled) {
    s_ambient_enabled = enabled;
    ceres_cyber_set_enabled(enabled);
}

bool ceres_ambient_is_enabled(void) {
    return s_ambient_enabled;
}

void ceres_ambient_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 256) volume = 256;
    s_master_vol = volume;
    ceres_cyber_set_volume((volume * 140) / 256);
}

int ceres_ambient_get_volume(void) {
    return s_master_vol;
}

void ceres_ambient_set_mood(ceres_ambient_mood_t mood) {
    if (mood >= CERES_AMBIENT_MOOD_COUNT) return;
    s_mood = mood;
    update_mood_tuning();
}

ceres_ambient_mood_t ceres_ambient_get_mood(void) {
    return s_mood;
}

void ceres_ambient_set_wavefold(int fold_drive) {
    if (fold_drive < 256) fold_drive = 256;
    if (fold_drive > 1024) fold_drive = 1024;
    s_target_fold_drive = fold_drive;
}

int ceres_ambient_get_wavefold(void) {
    return s_target_fold_drive;
}

/* Multi-stage Buchla wavefolder with 1-pole low-pass smoothing filter.
 * Pure fixed-point integer math (zero float, M-mode mstatus.FS = 0 safe). */
static inline int32_t ceres_wavefold(int32_t sample, int fold_drive, int32_t *filter_state) {
    if (fold_drive <= 256) {
        *filter_state = sample;
        return sample;
    }

    /* Drive input into folding threshold */
    int32_t x = (sample * fold_drive) >> 8;
    const int32_t THRESH = 18000;

    /* Multi-stage symmetrical folding */
    if (x > THRESH) {
        x = (2 * THRESH) - x;
        if (x < -THRESH) {
            x = -(2 * THRESH) - x;
            if (x > THRESH) {
                x = (2 * THRESH) - x;
            }
        }
    } else if (x < -THRESH) {
        x = -(2 * THRESH) - x;
        if (x > THRESH) {
            x = (2 * THRESH) - x;
            if (x < -THRESH) {
                x = -(2 * THRESH) - x;
            }
        }
    }

    /* 1-pole low-pass smoothing filter (eliminates harsh digital aliasing, warm analog tone) */
    *filter_state += (x - *filter_state) >> 2;
    return *filter_state;
}

void ceres_ambient_duck(int duck_amount) {
    if (duck_amount <= 0) return;
    s_duck_level += duck_amount;
    if (s_duck_level > 220) s_duck_level = 220; /* Keep subtle ambience audible even during heavy duck */
}

static void trigger_single_chime(uint32_t hz10) {
    /* Find an available chime voice, or steal the oldest decaying voice */
    chime_voice_t *chosen = NULL;
    for (int i = 0; i < CHIME_VOICES; i++) {
        if (!s_chimes[i].active) {
            chosen = &s_chimes[i];
            break;
        }
    }
    if (!chosen) {
        /* Steal lowest envelope voice */
        chosen = &s_chimes[0];
        for (int i = 1; i < CHIME_VOICES; i++) {
            if (s_chimes[i].env < chosen->env) {
                chosen = &s_chimes[i];
            }
        }
    }

    chosen->active = true;
    chosen->state = CHIME_STATE_ATTACK;
    chosen->phase_fund = 0;
    chosen->step_fund = AMBIENT_HZ10_TO_STEP(hz10);
    chosen->phase_over = 0;
    /* Inharmonic metallic chime overtone (~2.414x) */
    chosen->step_over = (chosen->step_fund * 241) / 100;
    chosen->env = 0;
    /* 40 ms soft attack (~882 samples) */
    chosen->env_attack = (65536 / (CERES_AMBIENT_SAMPLE_RATE * 40 / 1000));
    chosen->volume = 4800 + (int32_t)(ambient_rand() % 1600);
}

static void trigger_generative_chime(void) {
    /* Select note from current mood's modal pitch pool */
    uint32_t hz10;
    uint32_t r = ambient_rand();
    if (s_mood == CERES_AMBIENT_MOOD_CAVERN) {
        hz10 = s_scale_cavern[r % (sizeof(s_scale_cavern) / sizeof(s_scale_cavern[0]))];
    } else if (s_mood == CERES_AMBIENT_MOOD_TENSE) {
        hz10 = s_scale_tense[r % (sizeof(s_scale_tense) / sizeof(s_scale_tense[0]))];
    } else {
        hz10 = s_scale_deep_space[r % (sizeof(s_scale_deep_space) / sizeof(s_scale_deep_space[0]))];
    }

    trigger_single_chime(hz10);

    /* 30% chance of a twin chime starlight chord */
    if ((ambient_rand() % 10) < 3) {
        uint32_t r2 = ambient_rand();
        uint32_t hz10_b;
        if (s_mood == CERES_AMBIENT_MOOD_CAVERN) {
            hz10_b = s_scale_cavern[r2 % (sizeof(s_scale_cavern) / sizeof(s_scale_cavern[0]))];
        } else if (s_mood == CERES_AMBIENT_MOOD_TENSE) {
            hz10_b = s_scale_tense[r2 % (sizeof(s_scale_tense) / sizeof(s_scale_tense[0]))];
        } else {
            hz10_b = s_scale_deep_space[r2 % (sizeof(s_scale_deep_space) / sizeof(s_scale_deep_space[0]))];
        }
        trigger_single_chime(hz10_b);
    }

    /* Next chime interval: 1.2 to 2.5 seconds */
    s_chime_timer = (int32_t)(CERES_AMBIENT_SAMPLE_RATE * (12 + (ambient_rand() % 14)) / 10);
}

void ceres_ambient_render(int16_t *buffer, int num_samples) {
    if (!s_ambient_enabled || !buffer || num_samples <= 0) return;

    for (int i = 0; i < num_samples; i++) {
        /* 1. Sub-Bass Detuned Chorus Drone + Psychoacoustic Harmonics */
        int16_t s_a = ambient_sine((uint16_t)s_drone_phase_a);
        int16_t s_b = ambient_sine((uint16_t)s_drone_phase_b);
        int16_t s_oct = ambient_sine((uint16_t)s_drone_phase_oct);
        int16_t s_fifth = ambient_sine((uint16_t)s_drone_phase_fifth);
        s_drone_phase_a += s_drone_step_a;
        s_drone_phase_b += s_drone_step_b;
        s_drone_phase_oct += s_drone_step_oct;
        s_drone_phase_fifth += s_drone_step_fifth;
        /* 50% beating root + 30% octave + 20% fifth (psychoacoustic warmth on small speakers) */
        int32_t drone_sample = (((int32_t)(s_a + s_b) / 2) * 5 + (int32_t)s_oct * 3 + (int32_t)s_fifth * 2) / 10;

        /* 2. Harmonic Pad with LFO Breathing (2-voice chord) */
        int16_t s_pad_a = ambient_sine((uint16_t)s_pad_phase_a);
        int16_t s_pad_b = ambient_sine((uint16_t)s_pad_phase_b);
        int16_t s_pad_lfo = ambient_sine((uint16_t)s_pad_lfo_phase);
        s_pad_phase_a += s_pad_step_a;
        s_pad_phase_b += s_pad_step_b;
        s_pad_lfo_phase += s_pad_lfo_step;
        /* Map LFO [-32767..32767] to [12000..30000] */
        int32_t pad_gain = 12000 + (((int32_t)s_pad_lfo + 32768) * 18000 >> 16);
        int32_t pad_sample = (((int32_t)(s_pad_a + s_pad_b) / 2) * pad_gain) >> 15;

        /* Slew fold drive smoothly towards target (Buchla LPG vactrol slew) */
        if (s_current_fold_drive != s_target_fold_drive) {
            int delta = (s_target_fold_drive - s_current_fold_drive) >> 7;
            if (delta == 0) delta = (s_target_fold_drive > s_current_fold_drive) ? 1 : -1;
            s_current_fold_drive += delta;
        }

        /* West Coast Buchla wavefolding on pure sine drone and pad */
        drone_sample = ceres_wavefold(drone_sample, s_current_fold_drive, &s_fold_filter_drone);
        pad_sample   = ceres_wavefold(pad_sample,   s_current_fold_drive, &s_fold_filter_pad);

        /* 3. Celestial Solar Wind (Filtered Pink Noise) */
        uint32_t noise_bit = ((s_wind_lfsr >> 0) ^ (s_wind_lfsr >> 2) ^ (s_wind_lfsr >> 3) ^ (s_wind_lfsr >> 5)) & 1u;
        s_wind_lfsr = (s_wind_lfsr >> 1) | (noise_bit << 15);
        int32_t raw_noise = (int32_t)(int16_t)s_wind_lfsr;

        int16_t s_wind_lfo = ambient_sine((uint16_t)s_wind_lfo_phase);
        s_wind_lfo_phase += s_wind_lfo_step;
        /* Dynamic filter cutoff alpha: [8..24] */
        int32_t alpha = 8 + (((int32_t)s_wind_lfo + 32768) * 16 >> 16);
        s_wind_lp += ((raw_noise - s_wind_lp) * alpha) >> 8;
        int32_t wind_sample = s_wind_lp * 2;

        /* 4. Generative Crystalline Starlight Chimes */
        if (s_chime_timer > 0) {
            s_chime_timer--;
            if (s_chime_timer == 0) {
                trigger_generative_chime();
            }
        }

        int32_t chime_mix = 0;
        for (int c = 0; c < CHIME_VOICES; c++) {
            chime_voice_t *v = &s_chimes[c];
            if (!v->active) continue;

            int16_t sf = ambient_sine((uint16_t)v->phase_fund);
            int16_t so = ambient_sine((uint16_t)v->phase_over);
            v->phase_fund += v->step_fund;
            v->phase_over += v->step_over;

            /* Metallic chime mix: 70% fundamental + 30% inharmonic overtone */
            int32_t chime_osc = ((int32_t)sf * 7 + (int32_t)so * 3) / 10;
            int32_t voice_out = (chime_osc * (v->env >> 8)) >> 8;
            chime_mix += (voice_out * v->volume) >> 12;

            if (v->state == CHIME_STATE_ATTACK) {
                v->env += v->env_attack;
                if (v->env >= 65536) {
                    v->env = 65536;
                    v->state = CHIME_STATE_DECAY;
                }
            } else if (v->state == CHIME_STATE_DECAY) {
                /* Long natural crystalline exponential decay (~2.2 sec ring) */
                v->env -= (v->env >> 14) + 1;
                if (v->env <= 0) {
                    v->env = 0;
                    v->active = false;
                    v->state = CHIME_STATE_IDLE;
                }
            }
        }

        /* 5. Audio Ducking Smooth Recovery */
        if (s_duck_level > 0) {
            /* Decay ducking level back to zero (~1.5 sec recovery) */
            s_duck_level -= (s_duck_level >> 10) + 1;
            if (s_duck_level < 0) s_duck_level = 0;
        }

        /* Effective Master Volume with Ducking */
        int32_t effective_vol = (s_master_vol * (256 - s_duck_level)) >> 8;

        /* Balance Master Ambient Mix */
        int32_t ambient_sum = (drone_sample * 26) / 100 +  /* Warm psychoacoustic sub-bass */
                              (pad_sample   * 32) / 100 +  /* Breathing pad chord */
                              (wind_sample  * 14) / 100 +  /* Interstellar solar wind */
                              (chime_mix    * 28) / 100;   /* Crystalline stars */

        int32_t scaled_ambient = (ambient_sum * effective_vol) >> 8;

        /* Mix directly into buffer with saturation clipping */
        int32_t out = (int32_t)buffer[i] + scaled_ambient;
        if (out > 32767) out = 32767;
        else if (out < -32768) out = -32768;
        buffer[i] = (int16_t)out;
    }

    /* Mix reactive cybernetic sequencer voice */
    ceres_cyber_render(buffer, num_samples);
}
