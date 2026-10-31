/* user/moonbase/src/moon_cyber.c — Dynamic Cybernetic Voice & Sequencer
 *
 * Implements a reactive 16-step procedural West Coast synth voice that
 * dynamically adapts its tempo, rhythm, scale intervals, envelope duration,
 * and Buchla wavefolding based on colony life support, energy, and aerospace telemetry.
 *
 * Ambient West Coast Architecture:
 * 1. Pure Sine-Core Oscillator: silky fundamental tone without harsh 8-bit pulse pops.
 * 2. Multi-Stage Buchla Wavefolder: expands harmonics dynamically from clean wooden bells
 *    to reedy marimbas and snarling analog overdrive.
 * 3. 2-Voice Overlapping Polyphony: soft 15 ms attack and lush 350-500 ms decay tails,
 *    eliminating staccato dryness and creating a continuous flowing ambient ribbon.
 * 4. 4-Bar Phrasing & Turnaround Fills: A -> A -> B -> Fill phrase cycle.
 * 5. Contextual Modulations & Procedural LFSR Micro-Ornamentation.
 *
 * Concurrency: Synchronous execution within the single-task game session.
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "moon_cyber.h"
#include <string.h>

#define CYBER_VOICE_COUNT 2

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
static inline int16_t cyber_sine(uint16_t phase) {
    uint8_t idx = (uint8_t)(phase >> 8);
    uint8_t frac = (uint8_t)(phase & 0xFF);
    int32_t v0 = s_sin_tab[idx];
    int32_t v1 = s_sin_tab[(uint8_t)(idx + 1)];
    return (int16_t)(v0 + (((v1 - v0) * frac) >> 8));
}

/* Phase step table for semitones 0..31 (C4 @ 261.6 Hz through G6 @ 1568 Hz).
 * Formula: step = (freq_hz * 65536) / 16000. Pure precomputed integer constants. */
static const uint16_t s_note_steps[32] = {
    1072, 1135, 1203, 1274, 1350, 1430, 1515, 1606, /* C4  .. G4  */
    1701, 1802, 1909, 2023, 2143, 2271, 2406, 2549, /* G#4 .. D#5 */
    2700, 2861, 3031, 3211, 3402, 3604, 3819, 4046, /* E5  .. B5  */
    4287, 4541, 4811, 5098, 5401, 5722, 6062, 6422  /* C6  .. G6  */
};

/* Buchla-style wavefolder with 1-pole low-pass smoothing filter.
 * Pure fixed-point integer math (zero float, M-mode mstatus.FS = 0 safe). */
static inline int32_t cyber_wavefold(int32_t sample, int fold_drive, int32_t *filter_state) {
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

/* Pattern definition for each telemetry state across a 4-bar phrase (A -> A -> B -> Fill) */
typedef struct {
    uint8_t  target_bpm;
    uint8_t  decay_shift;       /* 11=~200ms, 12=~350ms, 13=~500ms lush ambient sustain */
    uint16_t fold_drive;        /* 256=1.0x (pure sine), 400=1.5x (reedy), 700+=heavy Buchla fold */
    int8_t   bar_a[16];         /* Bars 0 & 1: Core signature ostinato (-1 = REST) */
    uint16_t accents_a[16];     /* Velocity multiplier [0..420], 256 = nominal */
    int8_t   bar_b[16];         /* Bar 2: Rhythmic / syncopated variation */
    uint16_t accents_b[16];
    int8_t   bar_fill[16];      /* Bar 3: Turnaround fill */
    uint16_t accents_fill[16];
} cyber_preset_t;

static const cyber_preset_t s_presets[MOON_CYBER_STATE_COUNT] = {
    /* 0. MOON_CYBER_CALM (105 BPM): Relaxed laboratory pulse, C-minor pentatonic (pure warm sine bell) */
    [MOON_CYBER_CALM] = {
        .target_bpm = 105,
        .decay_shift = 13,       /* ~500 ms long ambient sustain */
        .fold_drive = 280,       /* Warm gentle harmonic bloom */
        .bar_a = {
            12, -1,  7, -1, 10, -1, 12, -1,
            15, -1, 12, -1,  7, -1, 10, -1
        },
        .accents_a = {
            280,   0, 200,   0, 240,   0, 260,   0,
            300,   0, 260,   0, 200,   0, 240,   0
        },
        .bar_b = {
            12, -1,  7, 10, -1, 12, -1, -1,
            15, -1, 12, 10, -1,  7, 10, -1
        },
        .accents_b = {
            280,   0, 200, 220,   0, 240,   0,   0,
            300,   0, 260, 220,   0, 200, 220,   0
        },
        .bar_fill = {
             7, 10, 12, 15, 17, 15, 12, 10,
            12, 15, 17, 19, 22, 19, 17, 15
        },
        .accents_fill = {
            240, 240, 260, 280, 300, 280, 260, 240,
            260, 280, 300, 320, 340, 320, 300, 280
        }
    },

    /* 1. MOON_CYBER_NIGHT_COLD (85 BPM): Sparse minimal clock in frozen habitat (crystalline overtones) */
    [MOON_CYBER_NIGHT_COLD] = {
        .target_bpm = 85,
        .decay_shift = 11,       /* Moderately long crisp chime */
        .fold_drive = 360,       /* Crystalline, icy overtones */
        .bar_a = {
             0, -1, -1, -1,  7, -1, -1, -1,
             3, -1, -1, -1,  0, -1, -1, -1
        },
        .accents_a = {
            260,   0,   0,   0, 200,   0,   0,   0,
            220,   0,   0,   0, 240,   0,   0,   0
        },
        .bar_b = {
             0, -1,  0, -1, -1,  7, -1, -1,
             3, -1, -1,  0, -1, -1,  0, -1
        },
        .accents_b = {
            260,   0, 200,   0,   0, 220,   0,   0,
            220,   0,   0, 200,   0,   0, 220,   0
        },
        .bar_fill = {
             0,  3,  7, 10, 12, 10,  7,  3,
             7, 10, 12, 15, 12, 10,  7,  3
        },
        .accents_fill = {
            220, 220, 240, 260, 280, 260, 240, 220,
            240, 260, 280, 300, 280, 260, 240, 220
        }
    },

    /* 2. MOON_CYBER_BROWNOUT (75 BPM): Syncopated sputtering energy warning (buzzy fold) */
    [MOON_CYBER_BROWNOUT] = {
        .target_bpm = 75,
        .decay_shift = 11,
        .fold_drive = 650,       /* Buzzing, unstable folded timbre */
        .bar_a = {
             0, -1,  0, -1, -1,  0, -1, -1,
             3, -1, -1,  0, -1, -1, -1, -1
        },
        .accents_a = {
            300,   0, 200,   0,   0, 240,   0,   0,
            260,   0,   0, 220,   0,   0,   0,   0
        },
        .bar_b = {
            -1,  0, -1, -1,  0, -1, -1, -1,
             3, -1,  0, -1, -1, -1,  0, -1
        },
        .accents_b = {
              0, 240,   0,   0, 220,   0,   0,   0,
            280,   0, 220,   0,   0,   0, 240,   0
        },
        .bar_fill = {
             0,  0, -1, -1,  3,  3, -1, -1,
             0, -1,  0, -1,  0, -1, -1, -1
        },
        .accents_fill = {
            280, 220,   0,   0, 300, 240,   0,   0,
            260,   0, 240,   0, 280,   0,   0,   0
        }
    },

    /* 3. MOON_CYBER_LIFE_CRITICAL (140 BPM): High-intensity red alert ostinato (Tritone Buchla siren) */
    [MOON_CYBER_LIFE_CRITICAL] = {
        .target_bpm = 140,
        .decay_shift = 12,       /* Resonant sustain */
        .fold_drive = 780,       /* Harsh, piercing Buchla fold harmonics */
        .bar_a = {
            18, 12, 18, 12, 17, 12, 17, 12,
            18, 12, 18, 12, 20, 19, 18, 17
        },
        .accents_a = {
            320, 220, 300, 220, 320, 220, 300, 220,
            340, 240, 320, 240, 320, 300, 280, 260
        },
        .bar_b = {
            20, 19, 18, 17, 18, 12, 18, 12,
            17, 12, 17, 12, 18, 12, 18, 12
        },
        .accents_b = {
            340, 320, 300, 280, 320, 220, 300, 220,
            320, 220, 300, 220, 340, 240, 320, 240
        },
        .bar_fill = {
            12, 15, 17, 18, 20, 21, 23, 24,
            24, 23, 21, 20, 18, 17, 15, 12
        },
        .accents_fill = {
            280, 300, 320, 340, 360, 380, 400, 420,
            420, 400, 380, 360, 340, 320, 300, 280
        }
    },

    /* 4. MOON_CYBER_STARSHIP_DESCENT (120 BPM): Driving aerospace approach rhythm (metallic marimba) */
    [MOON_CYBER_STARSHIP_DESCENT] = {
        .target_bpm = 120,
        .decay_shift = 12,
        .fold_drive = 420,       /* Rich resonant marimba */
        .bar_a = {
            12, 24, 19, 24, 15, 24, 19, 24,
            22, 24, 19, 24, 15, 24, 19, 24
        },
        .accents_a = {
            300, 220, 260, 220, 280, 220, 260, 220,
            300, 220, 260, 220, 280, 220, 260, 220
        },
        .bar_b = {
            12, 12, 24, 19, 15, -1, 24, 19,
            22, 22, 24, 19, 15, 24, 19, 24
        },
        .accents_b = {
            320, 240, 260, 220, 280,   0, 260, 220,
            320, 240, 260, 220, 280, 220, 260, 220
        },
        .bar_fill = {
            24, 22, 19, 15, 19, 15, 12, 10,
            15, 12, 10,  7, 12, 10,  7,  0
        },
        .accents_fill = {
            320, 300, 280, 260, 280, 260, 240, 220,
            260, 240, 220, 200, 240, 220, 200, 180
        }
    },

    /* 5. MOON_CYBER_STARSHIP_LAUNCH (150 BPM): Fast 2-octave ascent cascade (bright overtone shimmer) */
    [MOON_CYBER_STARSHIP_LAUNCH] = {
        .target_bpm = 150,
        .decay_shift = 12,
        .fold_drive = 520,       /* Bright ascending cascade shimmer */
        .bar_a = {
            12, 15, 19, 24, 27, 19, 24, 27,
            31, 27, 24, 19, 15, 19, 24, 27
        },
        .accents_a = {
            320, 240, 260, 300, 320, 260, 300, 320,
            340, 320, 300, 260, 240, 260, 300, 320
        },
        .bar_b = {
            24, 27, 31, 27, 24, 19, 24, 27,
            24, 27, 31, 27, 24, 19, 24, 27
        },
        .accents_b = {
            340, 300, 340, 300, 320, 260, 300, 320,
            340, 300, 340, 300, 320, 260, 300, 320
        },
        .bar_fill = {
            12, 15, 17, 19, 22, 24, 26, 27,
            29, 31, 31, 29, 27, 26, 24, 22
        },
        .accents_fill = {
            280, 300, 320, 340, 360, 380, 400, 420,
            440, 440, 420, 400, 380, 360, 340, 320
        }
    }
};

/* --- Synthesizer State (single-task game domain) --- */
/* single task: game */
static bool                s_enabled = true;
/* single task: game */
static int                 s_master_vol = 140;       /* [0..256], ~55% of full scale */
/* single task: game */
static moon_cyber_state_t  s_current_state = MOON_CYBER_CALM;
/* single task: game */
static moon_cyber_state_t  s_target_state  = MOON_CYBER_CALM;
/* single task: game */
static int                 s_current_bpm = 105;
/* single task: game */
static int                 s_target_bpm  = 105;

/* Sequencer clock and 4-bar phrase tracking */
/* single task: game */
static uint32_t            s_sample_clock = 0;
/* single task: game */
static uint32_t            s_samples_per_step = 2285;
/* single task: game */
static uint8_t             s_step_index = 0;
/* single task: game */
static uint8_t             s_bar_index = 0;          /* 0, 1 = Bar A, 2 = Bar B, 3 = Turnaround Fill */

/* 2-Voice Overlapping Polyphony State */
typedef enum {
    CYBER_VOICE_IDLE = 0,
    CYBER_VOICE_ATTACK,
    CYBER_VOICE_DECAY
} cyber_voice_state_t;

typedef struct {
    bool                active;
    cyber_voice_state_t state;
    uint16_t            phase;
    uint16_t            phase_step;
    int32_t             env;          /* 16.16 fixed-point [0..65536] */
    int32_t             env_attack;   /* increment per sample during attack */
    uint16_t            accent;       /* [0..420], 256 = nominal */
    uint8_t             decay_shift;  /* 11=~200ms, 12=~350ms, 13=~500ms */
    uint16_t            fold_drive;   /* [256..1024] */
    int32_t             filter_state;
} cyber_voice_t;

/* single task: game */
static cyber_voice_t       s_voices[CYBER_VOICE_COUNT];
/* single task: game */
static uint8_t             s_voice_turn = 0;         /* Alternating voice index for ping-pong polyphony */

/* Contextual telemetry cache */
/* single task: game */
static int32_t             s_power_net_kw = 0;
/* single task: game */
static uint8_t             s_life_support_pct = 100;
/* single task: game */
static bool                s_aerospace_active = false;

/* Deterministic LFSR for procedural micro-ornamentation */
/* single task: game */
static uint16_t            s_cyber_lfsr = 0x5AA5u;

void moon_cyber_init(void) {
    moon_cyber_reset();
}

void moon_cyber_reset(void) {
    s_enabled = true;
    s_master_vol = 140;
    s_current_state = MOON_CYBER_CALM;
    s_target_state  = MOON_CYBER_CALM;
    s_current_bpm = s_presets[MOON_CYBER_CALM].target_bpm;
    s_target_bpm  = s_presets[MOON_CYBER_CALM].target_bpm;
    s_samples_per_step = (MOON_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
    s_sample_clock = 0;
    s_step_index = 0;
    s_bar_index = 0;
    s_voice_turn = 0;
    memset(s_voices, 0, sizeof(s_voices));
    s_power_net_kw = 0;
    s_life_support_pct = 100;
    s_aerospace_active = false;
    s_cyber_lfsr = 0x5AA5u;
}

void moon_cyber_set_enabled(bool enabled) {
    s_enabled = enabled;
}

bool moon_cyber_is_enabled(void) {
    return s_enabled;
}

void moon_cyber_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 256) volume = 256;
    s_master_vol = volume;
}

int moon_cyber_get_volume(void) {
    return s_master_vol;
}

void moon_cyber_set_state(moon_cyber_state_t state) {
    if (state >= MOON_CYBER_STATE_COUNT) return;
    s_target_state = state;
    s_target_bpm = s_presets[state].target_bpm;
    if (!s_voices[0].active && !s_voices[1].active && s_sample_clock == 0) {
        s_current_state = state;
        s_current_bpm = s_target_bpm;
        s_samples_per_step = (MOON_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
    }
}

moon_cyber_state_t moon_cyber_get_state(void) {
    return s_target_state;
}

int moon_cyber_get_current_bpm(void) {
    return s_current_bpm;
}

uint8_t moon_cyber_get_bar_index(void) {
    return s_bar_index;
}

void moon_cyber_update_telemetry(const moon_cyber_telemetry_t *telem) {
    if (!telem) return;

    moon_cyber_state_t desired = MOON_CYBER_CALM;

    /* Priority 1: Starship launch return flight */
    if (telem->starship_state == 7 /* MOON_STARSHIP_ASCENT */) {
        desired = MOON_CYBER_STARSHIP_LAUNCH;
    }
    /* Priority 2: Starship / Lander descent approach */
    else if (telem->landing_active ||
             telem->starship_state == 1 /* MOON_STARSHIP_APPROACH */ ||
             telem->starship_state == 2 /* MOON_STARSHIP_RETRO */) {
        desired = MOON_CYBER_STARSHIP_DESCENT;
    }
    /* Priority 3: Life support emergency */
    else if (telem->life_support_pct < 40 ||
             telem->warning_o2 || telem->warning_water || telem->warning_cold) {
        desired = MOON_CYBER_LIFE_CRITICAL;
    }
    /* Priority 4: Power brownout or empty battery deficit */
    else if (telem->power_brownout ||
             (telem->battery_pct < 10 && telem->power_net_kw < 0)) {
        desired = MOON_CYBER_BROWNOUT;
    }
    /* Priority 5: Cryogenic lunar night */
    else if (telem->is_night) {
        desired = MOON_CYBER_NIGHT_COLD;
    }
    /* Default: Stable daylight colony operations */
    else {
        desired = MOON_CYBER_CALM;
    }

    /* Update contextual modulations */
    s_power_net_kw = telem->power_net_kw;
    s_life_support_pct = telem->life_support_pct;
    s_aerospace_active = (telem->landing_active || telem->starship_state > 0);

    moon_cyber_set_state(desired);
}

void moon_cyber_render(int16_t *buffer, int num_samples) {
    if (!s_enabled || !buffer || num_samples <= 0 || s_master_vol <= 0) return;

    for (int i = 0; i < num_samples; i++) {
        /* Advance step clock */
        s_sample_clock++;
        if (s_sample_clock >= s_samples_per_step) {
            s_sample_clock = 0;
            s_step_index = (s_step_index + 1) & 15;

            /* Advance 4-bar phrase counter on bar boundaries (step 0) */
            if (s_step_index == 0) {
                s_bar_index = (s_bar_index + 1) & 3;
            }

            /* Smoothly slew BPM towards target */
            if ((s_step_index & 3) == 0 && s_current_bpm != s_target_bpm) {
                int delta = (s_target_bpm - s_current_bpm) / 4;
                if (delta == 0) delta = (s_target_bpm > s_current_bpm) ? 1 : -1;
                s_current_bpm += delta;
                s_samples_per_step = (MOON_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
            }

            /* On bar boundaries (step 0), switch to target state seamlessly */
            if (s_step_index == 0 && s_current_state != s_target_state) {
                s_current_state = s_target_state;
            }

            /* 1. 4-Bar Phrasing: Select Pattern Slice (A -> A -> B -> Fill) */
            const cyber_preset_t *preset = &s_presets[s_current_state];
            const int8_t *notes_slice = preset->bar_a;
            const uint16_t *accents_slice = preset->accents_a;

            if (s_bar_index == 2) {
                notes_slice = preset->bar_b;
                accents_slice = preset->accents_b;
            } else if (s_bar_index == 3) {
                notes_slice = preset->bar_fill;
                accents_slice = preset->accents_fill;
            }

            int8_t note_idx = notes_slice[s_step_index];
            uint16_t accent = accents_slice[s_step_index];
            uint16_t fold = preset->fold_drive;
            uint8_t decay_shift = preset->decay_shift;

            /* 2. Contextual Telemetry Micro-Modulations */
            if (note_idx >= 0) {
                /* Power Net Surplus/Deficit: surplus boosts accent energy and fold harmonics, deficit softens */
                if (s_power_net_kw > 20) {
                    accent = (uint16_t)(((uint32_t)accent * 281u) >> 8); /* +10% */
                    fold += 80;
                } else if (s_power_net_kw < -20) {
                    accent = (uint16_t)(((uint32_t)accent * 230u) >> 8); /* -10% */
                    fold = (fold > 80) ? (fold - 60) : fold;
                }

                /* Aerospace Flight: Turnaround fill pitches up into higher octave */
                if (s_bar_index == 3 && s_aerospace_active && note_idx + 12 < 32) {
                    note_idx = (int8_t)(note_idx + 12);
                    fold += 100;
                }

                /* Life support stress warning accent */
                if (s_life_support_pct < 60 && (s_step_index == 3 || s_step_index == 7 || s_step_index == 11 || s_step_index == 15)) {
                    accent = (uint16_t)(((uint32_t)accent * 300u) >> 8);
                    fold += 100;
                }

                /* 3. Procedural Micro-Ornamentation (Galois LFSR) */
                s_cyber_lfsr = (uint16_t)((s_cyber_lfsr >> 1) ^ (-(int32_t)(s_cyber_lfsr & 1u) & 0xB400u));
                if (s_step_index == 7 || s_step_index == 15) {
                    /* ~12.5% chance of an octave-up grace sparkle */
                    if ((s_cyber_lfsr & 0x7u) == 1u && note_idx + 12 < 32) {
                        note_idx = (int8_t)(note_idx + 12);
                        accent = (uint16_t)(((uint32_t)accent * 290u) >> 8);
                    }
                    /* ~25% chance of subtle dynamic wavefold bloom */
                    if ((s_cyber_lfsr & 0x18u) == 0u) {
                        fold += 60;
                    }
                }

                /* Trigger active voice using 2-voice ping-pong polyphony */
                if (note_idx >= 0 && note_idx < 32) {
                    cyber_voice_t *v = &s_voices[s_voice_turn];
                    s_voice_turn = (s_voice_turn + 1) & (CYBER_VOICE_COUNT - 1);

                    v->active = true;
                    v->state = CYBER_VOICE_ATTACK;
                    v->phase = 0;
                    v->phase_step = s_note_steps[note_idx];
                    v->env = 0;
                    /* Soft ~15 ms attack (240 samples @ 16 kHz) eliminates staccato clicks */
                    v->env_attack = (65536 / (MOON_CYBER_SAMPLE_RATE * 15 / 1000));
                    v->accent = accent;
                    v->decay_shift = decay_shift;
                    v->fold_drive = fold;
                }
            }
        }

        /* Synthesize active voices with 2-voice overlapping polyphony */
        int32_t cyber_sum = 0;
        for (int v_idx = 0; v_idx < CYBER_VOICE_COUNT; v_idx++) {
            cyber_voice_t *v = &s_voices[v_idx];
            if (!v->active) continue;

            v->phase = (uint16_t)(v->phase + v->phase_step);
            int16_t raw_sine = cyber_sine(v->phase);

            /* Apply West Coast Buchla wavefolding to pure sine */
            int32_t folded = cyber_wavefold(raw_sine, v->fold_drive, &v->filter_state);

            /* Envelope VCA */
            int32_t env_amp = (v->env * (int32_t)v->accent) >> 8;
            int32_t voice_out = (folded * (env_amp >> 8)) >> 8;
            cyber_sum += voice_out;

            /* Advance Envelope: Attack -> Long ambient decay */
            if (v->state == CYBER_VOICE_ATTACK) {
                v->env += v->env_attack;
                if (v->env >= 65536) {
                    v->env = 65536;
                    v->state = CYBER_VOICE_DECAY;
                }
            } else if (v->state == CYBER_VOICE_DECAY) {
                /* Long flowing ambient decay tail (~350-500 ms) */
                v->env -= (v->env >> v->decay_shift) + 1;
                if (v->env <= 0) {
                    v->env = 0;
                    v->active = false;
                    v->state = CYBER_VOICE_IDLE;
                }
            }
        }

        /* Scale by master volume [0..256] with dual-voice headroom scaling */
        int32_t scaled = (cyber_sum * s_master_vol) >> 8;

        /* Accumulate with saturation into output stream */
        int32_t out = (int32_t)buffer[i] + scaled;
        if (out > 32767) out = 32767;
        else if (out < -32768) out = -32768;
        buffer[i] = (int16_t)out;
    }
}
