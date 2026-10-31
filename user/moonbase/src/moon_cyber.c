/* user/moonbase/src/moon_cyber.c — Dynamic Cybernetic Voice & Sequencer
 *
 * Implements a reactive 16-step procedural ostinato synthesizer voice that
 * dynamically adapts its tempo, rhythm, scale intervals, and pluck timbre
 * based on colony life support, energy, and aerospace telemetry.
 *
 * Concurrency: Synchronous execution within the single-task game session.
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "moon_cyber.h"
#include <string.h>

/* Phase step table for semitones 0..31 (C4 @ 261.6 Hz through G6 @ 1568 Hz).
 * Formula: step = (freq_hz * 65536) / 16000. Pure precomputed integer constants. */
static const uint16_t s_note_steps[32] = {
    1072, 1135, 1203, 1274, 1350, 1430, 1515, 1606, /* C4  .. G4  */
    1701, 1802, 1909, 2023, 2143, 2271, 2406, 2549, /* G#4 .. D#5 */
    2700, 2861, 3031, 3211, 3402, 3604, 3819, 4046, /* E5  .. B5  */
    4287, 4541, 4811, 5098, 5401, 5722, 6062, 6422  /* C6  .. G6  */
};

/* Pattern definition for each telemetry state */
typedef struct {
    uint8_t target_bpm;
    uint8_t decay_shift;       /* 8=dry click, 9=tight pluck, 10=ringing alarm */
    uint16_t duty_threshold;   /* 8192=12.5%, 16384=25%, 32768=50% */
    int8_t  notes[16];         /* Semitone index (0..31), -1 = REST */
    uint16_t accents[16];      /* Velocity multiplier [0..384], 256 = nominal */
} cyber_preset_t;

/* Presets designed for distinct psychological atmosphere and colony feedback */
static const cyber_preset_t s_presets[MOON_CYBER_STATE_COUNT] = {
    /* 0. MOON_CYBER_CALM (105 BPM): Relaxed laboratory pulse, C-minor pentatonic */
    [MOON_CYBER_CALM] = {
        .target_bpm = 105,
        .decay_shift = 9,
        .duty_threshold = 16384, /* 25% glassy pulse */
        .notes = {
            12, -1,  7, -1, 10, -1, 12, -1,
            15, -1, 12, -1,  7, -1, 10, -1
        },
        .accents = {
            320,   0, 240,   0, 280,   0, 300,   0,
            340,   0, 280,   0, 240,   0, 280,   0
        }
    },

    /* 1. MOON_CYBER_NIGHT_COLD (85 BPM): Sparse minimal clock in frozen habitat */
    [MOON_CYBER_NIGHT_COLD] = {
        .target_bpm = 85,
        .decay_shift = 8,        /* Very short dry mechanical tick */
        .duty_threshold = 32768, /* 50% hollow square */
        .notes = {
             0, -1, -1, -1,  7, -1, -1, -1,
             3, -1, -1, -1,  0, -1, -1, -1
        },
        .accents = {
            280,   0,   0,   0, 220,   0,   0,   0,
            240,   0,   0,   0, 260,   0,   0,   0
        }
    },

    /* 2. MOON_CYBER_BROWNOUT (75 BPM): Syncopated sputtering energy warning */
    [MOON_CYBER_BROWNOUT] = {
        .target_bpm = 75,
        .decay_shift = 8,
        .duty_threshold = 8192,  /* 12.5% thin robotic buzz */
        .notes = {
             0, -1,  0, -1, -1,  0, -1, -1,
             3, -1, -1,  0, -1, -1, -1, -1
        },
        .accents = {
            340,   0, 220,   0,   0, 260,   0,   0,
            300,   0,   0, 240,   0,   0,   0,   0
        }
    },

    /* 3. MOON_CYBER_LIFE_CRITICAL (140 BPM): High-intensity red alert ostinato (Tritone intervals) */
    [MOON_CYBER_LIFE_CRITICAL] = {
        .target_bpm = 140,
        .decay_shift = 10,       /* Long ringing resonant ping */
        .duty_threshold = 16384, /* 25% sharp alarm */
        .notes = {
            18, 12, 18, 12, 17, 12, 17, 12,
            18, 12, 18, 12, 20, 19, 18, 17
        },
        .accents = {
            360, 240, 340, 240, 360, 240, 340, 240,
            380, 260, 360, 260, 360, 340, 320, 300
        }
    },

    /* 4. MOON_CYBER_STARSHIP_DESCENT (120 BPM): Driving aerospace approach rhythm */
    [MOON_CYBER_STARSHIP_DESCENT] = {
        .target_bpm = 120,
        .decay_shift = 9,
        .duty_threshold = 16384,
        .notes = {
            12, 24, 19, 24, 15, 24, 19, 24,
            22, 24, 19, 24, 15, 24, 19, 24
        },
        .accents = {
            340, 260, 300, 260, 320, 260, 300, 260,
            340, 260, 300, 260, 320, 260, 300, 260
        }
    },

    /* 5. MOON_CYBER_STARSHIP_LAUNCH (150 BPM): Fast 2-octave ascent cascade */
    [MOON_CYBER_STARSHIP_LAUNCH] = {
        .target_bpm = 150,
        .decay_shift = 9,
        .duty_threshold = 16384,
        .notes = {
            12, 15, 19, 24, 27, 19, 24, 27,
            31, 27, 24, 19, 15, 19, 24, 27
        },
        .accents = {
            360, 280, 300, 340, 360, 300, 340, 360,
            384, 360, 340, 300, 280, 300, 340, 360
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

/* Sequencer clock */
/* single task: game */
static uint32_t            s_sample_clock = 0;
/* single task: game */
static uint32_t            s_samples_per_step = 2285;
/* single task: game */
static uint8_t             s_step_index = 0;

/* Active Voice State */
/* single task: game */
static bool                s_voice_active = false;
/* single task: game */
static uint16_t            s_voice_phase = 0;
/* single task: game */
static uint16_t            s_voice_phase_step = 0;
/* single task: game */
static int32_t             s_voice_env = 0;         /* 16.16 fixed-point [0..65536] */
/* single task: game */
static uint16_t            s_voice_accent = 256;
/* single task: game */
static uint8_t             s_current_decay_shift = 9;
/* single task: game */
static uint16_t            s_current_duty = 16384;
/* single task: game */
static int32_t             s_filter_state = 0;

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
    s_voice_active = false;
    s_voice_phase = 0;
    s_voice_phase_step = 0;
    s_voice_env = 0;
    s_filter_state = 0;
    s_current_decay_shift = s_presets[MOON_CYBER_CALM].decay_shift;
    s_current_duty = s_presets[MOON_CYBER_CALM].duty_threshold;
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
    if (!s_voice_active && s_sample_clock == 0) {
        s_current_state = state;
        s_current_bpm = s_target_bpm;
        s_current_decay_shift = s_presets[state].decay_shift;
        s_current_duty = s_presets[state].duty_threshold;
        s_samples_per_step = (MOON_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
    }
}

moon_cyber_state_t moon_cyber_get_state(void) {
    return s_target_state;
}

int moon_cyber_get_current_bpm(void) {
    return s_current_bpm;
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

            /* On step 0 (or every 4 steps), smoothly slew BPM towards target */
            if ((s_step_index & 3) == 0 && s_current_bpm != s_target_bpm) {
                int delta = (s_target_bpm - s_current_bpm) / 4;
                if (delta == 0) delta = (s_target_bpm > s_current_bpm) ? 1 : -1;
                s_current_bpm += delta;
                s_samples_per_step = (MOON_CYBER_SAMPLE_RATE * 60) / (s_current_bpm * 4);
            }

            /* On bar boundaries (step 0), switch to target state seamlessly */
            if (s_step_index == 0 && s_current_state != s_target_state) {
                s_current_state = s_target_state;
                s_current_decay_shift = s_presets[s_current_state].decay_shift;
                s_current_duty = s_presets[s_current_state].duty_threshold;
            }

            /* Trigger note for current step */
            const cyber_preset_t *preset = &s_presets[s_current_state];
            int8_t note_idx = preset->notes[s_step_index];
            if (note_idx >= 0 && note_idx < 32) {
                s_voice_active = true;
                s_voice_phase = 0;
                s_voice_phase_step = s_note_steps[note_idx];
                s_voice_env = 65536;
                s_voice_accent = preset->accents[s_step_index];
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
