/* user/ceres/src/ceres_cyber.c — Dynamic Cybernetic Voice & Meltdown Sequencer
 *
 * Implements a reactive 16-step procedural ostinato synthesizer voice that
 * dynamically adapts its tempo, rhythm, scale intervals, and pluck timbre
 * based on combat sector, fuel economy, survivor rescues, and base meltdown telemetry.
 *
 * Three Generative Variation Systems:
 * 1. 4-Bar Phrasing: A -> A -> B -> Turnaround Fill cycle preventing loop monotony.
 * 2. Contextual Telemetry Micro-Modulations: Thruster boost/brake dynamics, pitch-bank
 *    octave steering on turnaround fills, and rescue streak accent boosts.
 * 3. Procedural Micro-Ornamentation: 16-bit deterministic Galois LFSR grace notes,
 *    octave-up sparkles, and dynamic duty-cycle timbre morphing.
 *
 * Concurrency: Synchronous execution within the single-task ceres_game session.
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "ceres_cyber.h"
#include <string.h>

/* Phase step table for semitones 0..31 (C4 @ 261.6 Hz through G6 @ 1568 Hz).
 * Formula: step = (freq_hz * 65536) / 16000. Pure precomputed integer constants. */
static const uint16_t s_note_steps[32] = {
    1072, 1135, 1203, 1274, 1350, 1430, 1515, 1606, /* C4  .. G4  */
    1701, 1802, 1909, 2023, 2143, 2271, 2406, 2549, /* G#4 .. D#5 */
    2700, 2861, 3031, 3211, 3402, 3604, 3819, 4046, /* E5  .. B5  */
    4287, 4541, 4811, 5098, 5401, 5722, 6062, 6422  /* C6  .. G6  */
};

/* Pattern definition for each telemetry state across a 4-bar phrase (A -> A -> B -> Fill) */
typedef struct {
    uint8_t  target_bpm;
    uint8_t  decay_shift;       /* 8=dry click, 9=tight pluck, 10=ringing alarm */
    uint16_t duty_threshold;   /* 8192=12.5%, 16384=25%, 32768=50% */
    int8_t   bar_a[16];         /* Bars 0 & 1: Core signature ostinato (-1 = REST) */
    uint16_t accents_a[16];     /* Velocity multiplier [0..420], 256 = nominal */
    int8_t   bar_b[16];         /* Bar 2: Rhythmic / syncopated variation */
    uint16_t accents_b[16];
    int8_t   bar_fill[16];      /* Bar 3: Turnaround fill */
    uint16_t accents_fill[16];
} cyber_preset_t;

static const cyber_preset_t s_presets[CERES_CYBER_STATE_COUNT] = {
    /* 0. CERES_CYBER_PATROL (108 BPM): Title screen / calm cruising, E-minor pentatonic */
    [CERES_CYBER_PATROL] = {
        .target_bpm = 108,
        .decay_shift = 9,
        .duty_threshold = 16384, /* 25% glassy pulse */
        .bar_a = {
            16, -1, 11, -1, 14, -1, 16, -1,
            19, -1, 16, -1, 14, -1, 11, -1
        },
        .accents_a = {
            300,   0, 220,   0, 260,   0, 280,   0,
            320,   0, 280,   0, 240,   0, 220,   0
        },
        .bar_b = {
            16, -1, 11, 14, -1, 16, -1, -1,
            19, -1, 16, 14, -1, 11, 14, -1
        },
        .accents_b = {
            320,   0, 240, 260,   0, 280,   0,   0,
            340,   0, 280, 260,   0, 240, 260,   0
        },
        .bar_fill = {
            11, 14, 16, 19, 21, 19, 16, 14,
            16, 19, 21, 23, 24, 23, 21, 19
        },
        .accents_fill = {
            260, 260, 280, 300, 320, 300, 280, 260,
            280, 300, 320, 340, 360, 340, 320, 300
        }
    },

    /* 1. CERES_CYBER_COMBAT (128 BPM): Sectors 1-3 Cavern & Trench cyber-runner drive */
    [CERES_CYBER_COMBAT] = {
        .target_bpm = 128,
        .decay_shift = 9,
        .duty_threshold = 16384, /* 25% snappy pulse */
        .bar_a = {
            16, -1, 16, 21, 19, 16, 23, 21,
            16, -1, 16, 21, 19, 21, 23, 24
        },
        .accents_a = {
            340,   0, 260, 300, 280, 260, 320, 280,
            340,   0, 260, 300, 280, 300, 340, 360
        },
        .bar_b = {
            16, 16, -1, 21, 19, -1, 23, 21,
            16, 16, -1, 21, 19, 23, 21, 19
        },
        .accents_b = {
            360, 280,   0, 300, 280,   0, 340, 280,
            360, 280,   0, 300, 280, 320, 300, 280
        },
        .bar_fill = {
            16, 19, 21, 23, 24, 26, 24, 23,
            21, 23, 24, 26, 28, 26, 24, 21
        },
        .accents_fill = {
            320, 320, 340, 340, 360, 360, 340, 320,
            320, 340, 360, 380, 400, 380, 360, 340
        }
    },

    /* 2. CERES_CYBER_STRONGHOLD (140 BPM): Sectors 4-6 Fortified Base & Labyrinth (Phrygian tension) */
    [CERES_CYBER_STRONGHOLD] = {
        .target_bpm = 140,
        .decay_shift = 9,
        .duty_threshold = 16384, /* 25% razor pulse */
        .bar_a = {
            16, 17, 16, -1, 19, 16, 17, 16,
            21, 17, 16, -1, 20, 19, 17, 16
        },
        .accents_a = {
            360, 280, 260,   0, 320, 260, 300, 260,
            340, 280, 260,   0, 340, 320, 300, 280
        },
        .bar_b = {
            17, 16, -1, 17, 16, -1, 20, 19,
            17, 16, -1, 17, 21, 20, 19, 17
        },
        .accents_b = {
            360, 280,   0, 320, 260,   0, 340, 300,
            340, 280,   0, 320, 360, 340, 320, 300
        },
        .bar_fill = {
            21, 20, 19, 18, 17, 16, 17, 18,
            19, 20, 21, 22, 23, 22, 21, 20
        },
        .accents_fill = {
            360, 340, 320, 300, 280, 260, 280, 300,
            320, 340, 360, 380, 400, 380, 360, 340
        }
    },

    /* 3. CERES_CYBER_SURVIVOR (120 BPM): Resonant high-register sonar beacon & parachute rescue */
    [CERES_CYBER_SURVIVOR] = {
        .target_bpm = 120,
        .decay_shift = 10,       /* Long ringing crystalline ping */
        .duty_threshold = 8192,  /* 12.5% narrow crystal ping */
        .bar_a = {
            28, -1, -1, 28, -1, -1, 31, -1,
            28, -1, -1, 28, -1, 26, -1, -1
        },
        .accents_a = {
            380,   0,   0, 260,   0,   0, 360,   0,
            380,   0,   0, 260,   0, 300,   0,   0
        },
        .bar_b = {
            28, -1, 28, -1, -1, 31, -1, -1,
            28, -1, 28, -1, 26, -1, 31, -1
        },
        .accents_b = {
            380,   0, 280,   0,   0, 360,   0,   0,
            380,   0, 280,   0, 320,   0, 360,   0
        },
        .bar_fill = {
            24, 26, 28, 31, 28, 31, 28, 26,
            28, 31, 28, 31, 31, 28, 26, 24
        },
        .accents_fill = {
            320, 340, 360, 380, 360, 380, 360, 340,
            360, 380, 360, 380, 400, 380, 360, 340
        }
    },

    /* 4. CERES_CYBER_LOW_FUEL (75 BPM): Sputtering broken meter / thruster flameout warning */
    [CERES_CYBER_LOW_FUEL] = {
        .target_bpm = 75,
        .decay_shift = 8,        /* Dry mechanical click */
        .duty_threshold = 32768, /* 50% hollow square */
        .bar_a = {
             4, -1,  4, -1, -1,  4, -1, -1,
             7, -1, -1,  4, -1, -1, -1, -1
        },
        .accents_a = {
            340,   0, 220,   0,   0, 260,   0,   0,
            300,   0,   0, 240,   0,   0,   0,   0
        },
        .bar_b = {
            -1,  4, -1, -1,  4, -1, -1, -1,
             7, -1,  4, -1, -1, -1,  4, -1
        },
        .accents_b = {
              0, 280,   0,   0, 240,   0,   0,   0,
            320,   0, 260,   0,   0,   0, 280,   0
        },
        .bar_fill = {
             4,  4, -1, -1,  7,  7, -1, -1,
             4, -1,  4, -1,  4, -1, -1, -1
        },
        .accents_fill = {
            320, 240,   0,   0, 340, 260,   0,   0,
            280,   0, 260,   0, 300,   0,   0,   0
        }
    },

    /* 5. CERES_CYBER_MELTDOWN (175 BPM): Sector 6 base meltdown emergency escape arpeggio */
    [CERES_CYBER_MELTDOWN] = {
        .target_bpm = 175,
        .decay_shift = 10,       /* Resonant sharp alarm ring */
        .duty_threshold = 8192,  /* 12.5% narrow laser alarm */
        .bar_a = {
            16, 17, 18, 19, 20, 21, 22, 23,
            24, 23, 22, 21, 20, 19, 18, 17
        },
        .accents_a = {
            380, 280, 300, 320, 340, 360, 380, 400,
            420, 380, 360, 340, 320, 300, 280, 260
        },
        .bar_b = {
            24, 23, 22, 21, 20, 19, 18, 17,
            16, 17, 18, 19, 20, 21, 22, 23
        },
        .accents_b = {
            420, 380, 360, 340, 320, 300, 280, 260,
            380, 280, 300, 320, 340, 360, 380, 400
        },
        .bar_fill = {
            24, 25, 26, 27, 28, 29, 30, 31,
            31, 30, 29, 28, 27, 26, 25, 24
        },
        .accents_fill = {
            400, 340, 360, 380, 400, 420, 440, 460,
            460, 440, 420, 400, 380, 360, 340, 320
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

/* Active Voice State */
/* single task: ceres_game */
static bool                 s_voice_active = false;
/* single task: ceres_game */
static uint16_t             s_voice_phase = 0;
/* single task: ceres_game */
static uint16_t             s_voice_phase_step = 0;
/* single task: ceres_game */
static int32_t              s_voice_env = 0;         /* 16.16 fixed-point [0..65536] */
/* single task: ceres_game */
static uint16_t             s_voice_accent = 256;
/* single task: ceres_game */
static uint8_t              s_current_decay_shift = 9;
/* single task: ceres_game */
static uint16_t             s_current_duty = 16384;
/* single task: ceres_game */
static int32_t              s_filter_state = 0;

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
    s_voice_active = false;
    s_voice_phase = 0;
    s_voice_phase_step = 0;
    s_voice_env = 0;
    s_filter_state = 0;
    s_thrust_boost = 0;
    s_pitch_bank = 0;
    s_rescue_pulse = false;
    s_cyber_lfsr = 0xACE1u;
    s_current_decay_shift = s_presets[CERES_CYBER_PATROL].decay_shift;
    s_current_duty = s_presets[CERES_CYBER_PATROL].duty_threshold;
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
    if (!s_voice_active && s_sample_clock == 0) {
        s_current_state = state;
        s_current_bpm = s_target_bpm;
        s_current_decay_shift = s_presets[state].decay_shift;
        s_current_duty = s_presets[state].duty_threshold;
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
                s_current_decay_shift = s_presets[s_current_state].decay_shift;
                s_current_duty = s_presets[s_current_state].duty_threshold;
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
            uint16_t duty = preset->duty_threshold;
            uint8_t decay_shift = preset->decay_shift;

            /* 2. Contextual Telemetry Micro-Modulations */
            if (note_idx >= 0) {
                /* Thruster Boost: forward boost tightens decay and enhances accents; braking softens */
                if (s_thrust_boost > 0) {
                    accent = (uint16_t)(((uint32_t)accent * 294u) >> 8); /* +15% */
                    if (decay_shift > 8) decay_shift--;
                } else if (s_thrust_boost < 0) {
                    accent = (uint16_t)(((uint32_t)accent * 218u) >> 8); /* -15% */
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
                }

                /* 3. Procedural Micro-Ornamentation (Galois LFSR) */
                s_cyber_lfsr = (uint16_t)((s_cyber_lfsr >> 1) ^ (-(int32_t)(s_cyber_lfsr & 1u) & 0xB400u));
                if (s_step_index == 7 || s_step_index == 15) {
                    /* ~12.5% chance of an octave-up grace sparkle */
                    if ((s_cyber_lfsr & 0x7u) == 1u && note_idx + 12 < 32) {
                        note_idx = (int8_t)(note_idx + 12);
                        accent = (uint16_t)(((uint32_t)accent * 300u) >> 8);
                    }
                    /* ~25% chance of dynamic duty-cycle timbre morph */
                    if ((s_cyber_lfsr & 0x18u) == 0u) {
                        duty = 8192; /* 12.5% bright crystalline pulse */
                    }
                }

                /* Trigger active synthesized voice */
                if (note_idx >= 0 && note_idx < 32) {
                    s_voice_active = true;
                    s_voice_phase = 0;
                    s_voice_phase_step = s_note_steps[note_idx];
                    s_voice_env = 65536;
                    s_voice_accent = accent;
                    s_current_duty = duty;
                    s_current_decay_shift = decay_shift;
                }
            }
        }

        /* Synthesize active voice */
        int32_t cyber_sample = 0;
        if (s_voice_active && s_voice_env > 0) {
            s_voice_phase = (uint16_t)(s_voice_phase + s_voice_phase_step);

            /* Pulse wave with state-dependent duty cycle */
            int32_t osc = (s_voice_phase < s_current_duty) ? 14000 : -14000;

            /* 1-pole low-pass smoothing filter (cuts aliasing & clicks) */
            s_filter_state += (osc - s_filter_state) >> 2;
            int32_t filtered = s_filter_state;

            /* Exponential pluck envelope */
            int32_t env_amp = (s_voice_env * (int32_t)s_voice_accent) >> 8;
            cyber_sample = (filtered * (env_amp >> 8)) >> 8;

            /* Decay calculation */
            s_voice_env -= (s_voice_env >> s_current_decay_shift) + 1;
            if (s_voice_env <= 0) {
                s_voice_env = 0;
                s_voice_active = false;
            }
        }

        /* Scale by master volume [0..256] */
        int32_t scaled = (cyber_sample * s_master_vol) >> 8;

        /* Accumulate with saturation into output stream */
        int32_t out = (int32_t)buffer[i] + scaled;
        if (out > 32767) out = 32767;
        else if (out < -32768) out = -32768;
        buffer[i] = (int16_t)out;
    }
}
