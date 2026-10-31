/* user/ceres/src/ceres_cyber.c — Dynamic Cybernetic Voice & Meltdown Sequencer
 *
 * Implements a reactive 16-step procedural West Coast synth voice that
 * dynamically adapts its tempo, rhythm, scale intervals, envelope duration,
 * and Buchla wavefolding based on combat sector, fuel economy, survivor rescues,
 * and base meltdown telemetry.
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
 * Concurrency: Synchronous execution within the single-task ceres_game session.
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "ceres_cyber.h"
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

static const cyber_preset_t s_presets[CERES_CYBER_STATE_COUNT] = {
    /* 0. CERES_CYBER_PATROL (108 BPM): Title screen / calm cruising, E-minor pentatonic (lush sine bell) */
    [CERES_CYBER_PATROL] = {
        .target_bpm = 108,
        .decay_shift = 13,       /* ~500 ms long ambient sustain */
        .fold_drive = 280,       /* Warm, gentle harmonic bloom */
        .bar_a = {
            16, -1, 11, -1, 14, -1, 16, -1,
            19, -1, 16, -1, 14, -1, 11, -1
        },
        .accents_a = {
            280,   0, 200,   0, 240,   0, 260,   0,
            300,   0, 260,   0, 220,   0, 200,   0
        },
        .bar_b = {
            16, -1, 11, 14, -1, 16, -1, -1,
            19, -1, 16, 14, -1, 11, 14, -1
        },
        .accents_b = {
            280,   0, 220, 240,   0, 260,   0,   0,
            300,   0, 260, 240,   0, 220, 240,   0
        },
        .bar_fill = {
            11, 14, 16, 19, 21, 19, 16, 14,
            16, 19, 21, 23, 24, 23, 21, 19
        },
        .accents_fill = {
            240, 240, 260, 280, 300, 280, 260, 240,
            260, 280, 300, 320, 340, 320, 300, 280
        }
    },

    /* 1. CERES_CYBER_COMBAT (128 BPM): Sectors 1-3 Cavern & Trench cyber-runner drive (reedy marimba) */
    [CERES_CYBER_COMBAT] = {
        .target_bpm = 128,
        .decay_shift = 12,       /* ~350 ms flowing tail */
        .fold_drive = 420,       /* Reedy, acoustic wavefolded resonance */
        .bar_a = {
            16, -1, 16, 21, 19, 16, 23, 21,
            16, -1, 16, 21, 19, 21, 23, 24
        },
        .accents_a = {
            320,   0, 240, 280, 260, 240, 300, 260,
            320,   0, 240, 280, 260, 280, 320, 340
        },
        .bar_b = {
            16, 16, -1, 21, 19, -1, 23, 21,
            16, 16, -1, 21, 19, 23, 21, 19
        },
        .accents_b = {
            340, 260,   0, 280, 260,   0, 320, 260,
            340, 260,   0, 280, 260, 300, 280, 260
        },
        .bar_fill = {
            16, 19, 21, 23, 24, 26, 24, 23,
            21, 23, 24, 26, 28, 26, 24, 21
        },
        .accents_fill = {
            300, 300, 320, 320, 340, 340, 320, 300,
            300, 320, 340, 360, 380, 360, 340, 320
        }
    },

    /* 2. CERES_CYBER_STRONGHOLD (140 BPM): Sectors 4-6 Fortified Base & Labyrinth (Phrygian tension) */
    [CERES_CYBER_STRONGHOLD] = {
        .target_bpm = 140,
        .decay_shift = 12,
        .fold_drive = 540,       /* Tense subterranean metallic overtones */
        .bar_a = {
            16, 17, 16, -1, 19, 16, 17, 16,
            21, 17, 16, -1, 20, 19, 17, 16
        },
        .accents_a = {
            340, 260, 240,   0, 300, 240, 280, 240,
            320, 260, 240,   0, 320, 300, 280, 260
        },
        .bar_b = {
            17, 16, -1, 17, 16, -1, 20, 19,
            17, 16, -1, 17, 21, 20, 19, 17
        },
        .accents_b = {
            340, 260,   0, 300, 240,   0, 320, 280,
            320, 260,   0, 300, 340, 320, 300, 280
        },
        .bar_fill = {
            21, 20, 19, 18, 17, 16, 17, 18,
            19, 20, 21, 22, 23, 22, 21, 20
        },
        .accents_fill = {
            340, 320, 300, 280, 260, 240, 260, 280,
            300, 320, 340, 360, 380, 360, 340, 320
        }
    },

    /* 3. CERES_CYBER_SURVIVOR (120 BPM): High-register resonant crystal sonar beacon */
    [CERES_CYBER_SURVIVOR] = {
        .target_bpm = 120,
        .decay_shift = 13,       /* Long ringing crystalline ambient tail */
        .fold_drive = 320,       /* Glassy harmonic shimmer */
        .bar_a = {
            28, -1, -1, 28, -1, -1, 31, -1,
            28, -1, -1, 28, -1, 26, -1, -1
        },
        .accents_a = {
            360,   0,   0, 240,   0,   0, 340,   0,
            360,   0,   0, 240,   0, 280,   0,   0
        },
        .bar_b = {
            28, -1, 28, -1, -1, 31, -1, -1,
            28, -1, 28, -1, 26, -1, 31, -1
        },
        .accents_b = {
            360,   0, 260,   0,   0, 340,   0,   0,
            360,   0, 260,   0, 300,   0, 340,   0
        },
        .bar_fill = {
            24, 26, 28, 31, 28, 31, 28, 26,
            28, 31, 28, 31, 31, 28, 26, 24
        },
        .accents_fill = {
            300, 320, 340, 360, 340, 360, 340, 320,
            340, 360, 340, 360, 380, 360, 340, 320
        }
    },

    /* 4. CERES_CYBER_LOW_FUEL (75 BPM): Sputtering broken meter / thruster flameout warning */
    [CERES_CYBER_LOW_FUEL] = {
        .target_bpm = 75,
        .decay_shift = 11,       /* Moderately dry hesitation */
        .fold_drive = 620,       /* Buzzing, unstable square wavefold */
        .bar_a = {
             4, -1,  4, -1, -1,  4, -1, -1,
             7, -1, -1,  4, -1, -1, -1, -1
        },
        .accents_a = {
            320,   0, 200,   0,   0, 240,   0,   0,
            280,   0,   0, 220,   0,   0,   0,   0
        },
        .bar_b = {
            -1,  4, -1, -1,  4, -1, -1, -1,
             7, -1,  4, -1, -1, -1,  4, -1
        },
        .accents_b = {
              0, 260,   0,   0, 220,   0,   0,   0,
            300,   0, 240,   0,   0,   0, 260,   0
        },
        .bar_fill = {
             4,  4, -1, -1,  7,  7, -1, -1,
             4, -1,  4, -1,  4, -1, -1, -1
        },
        .accents_fill = {
            300, 220,   0,   0, 320, 240,   0,   0,
            260,   0, 240,   0, 280,   0,   0,   0
        }
    },

    /* 5. CERES_CYBER_MELTDOWN (175 BPM): Sector 6 base meltdown emergency escape arpeggio */
    [CERES_CYBER_MELTDOWN] = {
        .target_bpm = 175,
        .decay_shift = 12,
        .fold_drive = 850,       /* Heavy searing Buchla overdrive harmonics */
        .bar_a = {
            16, 17, 18, 19, 20, 21, 22, 23,
            24, 23, 22, 21, 20, 19, 18, 17
        },
        .accents_a = {
            360, 260, 280, 300, 320, 340, 360, 380,
            400, 360, 340, 320, 300, 280, 260, 240
        },
        .bar_b = {
            24, 23, 22, 21, 20, 19, 18, 17,
            16, 17, 18, 19, 20, 21, 22, 23
        },
        .accents_b = {
            400, 360, 340, 320, 300, 280, 260, 240,
            360, 260, 280, 300, 320, 340, 360, 380
        },
        .bar_fill = {
            24, 25, 26, 27, 28, 29, 30, 31,
            31, 30, 29, 28, 27, 26, 25, 24
        },
        .accents_fill = {
            380, 320, 340, 360, 380, 400, 420, 440,
            440, 420, 400, 380, 360, 340, 320, 300
        }
    }
};

/* --- Synthesizer State (single-task ceres_game domain) --- */
/* single task: ceres_game */
static bool                 s_enabled = true;
/* single task: ceres_game */
static int                  s_master_vol = 140;       /* [0..256], ~55% of full scale */
/* single task: ceres_game */
static ceres_cyber_state_t  s_current_state = CERES_CYBER_PATROL;
/* single task: ceres_game */
static ceres_cyber_state_t  s_target_state  = CERES_CYBER_PATROL;
/* single task: ceres_game */
static int                  s_current_bpm = 108;
/* single task: ceres_game */
static int                  s_target_bpm  = 108;

/* Sequencer clock and 4-bar phrase tracking */
/* single task: ceres_game */
static uint32_t             s_sample_clock = 0;
/* single task: ceres_game */
static uint32_t             s_samples_per_step = 2222;
/* single task: ceres_game */
static uint8_t              s_step_index = 0;
/* single task: ceres_game */
static uint8_t              s_bar_index = 0;          /* 0, 1 = Bar A, 2 = Bar B, 3 = Turnaround Fill */

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

/* single task: ceres_game */
static cyber_voice_t        s_voices[CYBER_VOICE_COUNT];
/* single task: ceres_game */
static uint8_t              s_voice_turn = 0;         /* Alternating voice index for ping-pong polyphony */

/* Contextual telemetry cache */
/* single task: ceres_game */
static int8_t               s_thrust_boost = 0;      /* -1 = brake, 0 = normal, +1 = forward */
/* single task: ceres_game */
static int8_t               s_pitch_bank = 0;        /* -1 = dive, 0 = level, +1 = climb */
/* single task: ceres_game */
static bool                 s_rescue_pulse = false;

/* Deterministic LFSR for procedural micro-ornamentation */
/* single task: ceres_game */
static uint16_t             s_cyber_lfsr = 0xACE1u;

void ceres_cyber_init(void) {
    ceres_cyber_reset();
}

void ceres_cyber_reset(void) {
    s_enabled = true;
    s_master_vol = 140;
    s_current_state = CERES_CYBER_PATROL;
    s_target_state  = CERES_CYBER_PATROL;
    s_current_bpm = s_presets[CERES_CYBER_PATROL].target_bpm;
    s_target_bpm  = s_presets[CERES_CYBER_PATROL].target_bpm;
    s_samples_per_step = (CERES_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
    s_sample_clock = 0;
    s_step_index = 0;
    s_bar_index = 0;
    s_voice_turn = 0;
    memset(s_voices, 0, sizeof(s_voices));
    s_thrust_boost = 0;
    s_pitch_bank = 0;
    s_rescue_pulse = false;
    s_cyber_lfsr = 0xACE1u;
}

void ceres_cyber_set_enabled(bool enabled) {
    s_enabled = enabled;
}

bool ceres_cyber_is_enabled(void) {
    return s_enabled;
}

void ceres_cyber_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 256) volume = 256;
    s_master_vol = volume;
}

int ceres_cyber_get_volume(void) {
    return s_master_vol;
}

void ceres_cyber_set_state(ceres_cyber_state_t state) {
    if (state >= CERES_CYBER_STATE_COUNT) return;
    s_target_state = state;
    s_target_bpm = s_presets[state].target_bpm;
    if (!s_voices[0].active && !s_voices[1].active && s_sample_clock == 0) {
        s_current_state = state;
        s_current_bpm = s_target_bpm;
        s_samples_per_step = (CERES_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
    }
}

ceres_cyber_state_t ceres_cyber_get_state(void) {
    return s_target_state;
}

int ceres_cyber_get_current_bpm(void) {
    return s_current_bpm;
}

uint8_t ceres_cyber_get_bar_index(void) {
    return s_bar_index;
}

void ceres_cyber_update_telemetry(const ceres_cyber_telemetry_t *telem) {
    if (!telem) return;

    ceres_cyber_state_t desired = CERES_CYBER_PATROL;

    /* Priority 1: Sector 6 Reactor Core detonated - emergency sprint */
    if (telem->meltdown_timer > 0) {
        desired = CERES_CYBER_MELTDOWN;
    }
    /* Priority 2: Fuel starvation alert (<20% or engine flameout) */
    else if (telem->low_fuel_alert || (telem->fuel < 200)) {
        desired = CERES_CYBER_LOW_FUEL;
    }
    /* Priority 3: Stranded survivor on screen / tractor scoop / parachute */
    else if (telem->tractor_active || telem->survivor_visible) {
        desired = CERES_CYBER_SURVIVOR;
    }
    /* Priority 4: Fortified Base, Labyrinth, or Stronghold (Sectors 4..6) */
    else if (telem->sector >= 4) {
        desired = CERES_CYBER_STRONGHOLD;
    }
    /* Priority 5: Active arcade combat runner (Sectors 1..3) */
    else if (telem->game_active) {
        desired = CERES_CYBER_COMBAT;
    }
    /* Default: Title screen / quiet patrolling */
    else {
        desired = CERES_CYBER_PATROL;
    }

    /* Update contextual modulations */
    s_thrust_boost = telem->thrust_boost;
    s_pitch_bank = telem->pitch_bank;
    if (telem->rescue_event) {
        s_rescue_pulse = true;
    }

    ceres_cyber_set_state(desired);
}

void ceres_cyber_render(int16_t *buffer, int num_samples) {
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
                s_rescue_pulse = false;
            }

            /* Smoothly slew BPM towards target */
            if ((s_step_index & 3) == 0 && s_current_bpm != s_target_bpm) {
                int delta = (s_target_bpm - s_current_bpm) / 4;
                if (delta == 0) delta = (s_target_bpm > s_current_bpm) ? 1 : -1;
                s_current_bpm += delta;
                s_samples_per_step = (CERES_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
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
                /* Thruster Boost: forward boost increases wavefold drive and accent energy; braking softens */
                if (s_thrust_boost > 0) {
                    accent = (uint16_t)(((uint32_t)accent * 294u) >> 8); /* +15% */
                    fold += 80; /* Brighter, richer harmonic edge under acceleration */
                } else if (s_thrust_boost < 0) {
                    accent = (uint16_t)(((uint32_t)accent * 218u) >> 8); /* -15% */
                    if (fold > 280) fold -= 40;
                }

                /* Pitch Bank: Pitching up transposes turnaround fill upward; diving grounds into bass */
                if (s_bar_index == 3) {
                    if (s_pitch_bank > 0 && note_idx + 12 < 32) {
                        note_idx = (int8_t)(note_idx + 12);
                    } else if (s_pitch_bank < 0 && note_idx >= 12) {
                        note_idx = (int8_t)(note_idx - 12);
                    }
                }

                /* Rescue streak sparkle */
                if (s_rescue_pulse && (s_step_index == 3 || s_step_index == 7 || s_step_index == 11 || s_step_index == 15)) {
                    accent = (uint16_t)(((uint32_t)accent * 340u) >> 8);
                    fold += 100;
                }

                /* 3. Procedural Micro-Ornamentation (Galois LFSR) */
                s_cyber_lfsr = (uint16_t)((s_cyber_lfsr >> 1) ^ (-(int32_t)(s_cyber_lfsr & 1u) & 0xB400u));
                if (s_step_index == 7 || s_step_index == 15) {
                    /* ~12.5% chance of an octave-up grace sparkle */
                    if ((s_cyber_lfsr & 0x7u) == 1u && note_idx + 12 < 32) {
                        note_idx = (int8_t)(note_idx + 12);
                        accent = (uint16_t)(((uint32_t)accent * 300u) >> 8);
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
                    v->env_attack = (65536 / (CERES_CYBER_SAMPLE_RATE * 15 / 1000));
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
