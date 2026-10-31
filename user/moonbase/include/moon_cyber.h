/* user/moonbase/include/moon_cyber.h — Dynamic Cybernetic Voice & Sequencer
 *
 * Implements a reactive 16-step procedural ostinato synthesizer voice that
 * dynamically adapts its tempo, rhythm, scale intervals, and pluck timbre
 * based on colony life support, energy, and aerospace telemetry.
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#ifndef MOON_CYBER_H
#define MOON_CYBER_H

#include <stdint.h>
#include <stdbool.h>

#define MOON_CYBER_SAMPLE_RATE 16000

/* Telemetry states governing the cybernetic track */
typedef enum {
    MOON_CYBER_CALM = 0,         /* Daylight, ample energy, nominal life support (105 BPM laboratory pulse) */
    MOON_CYBER_NIGHT_COLD,       /* Frozen lunar night, battery discharge (85 BPM muted minimal ticking) */
    MOON_CYBER_BROWNOUT,         /* Depleted power / blackout risk (75 BPM syncopated stutter) */
    MOON_CYBER_LIFE_CRITICAL,    /* Life support critical (<40%), low O2/water (140 BPM urgent warning alarm) */
    MOON_CYBER_STARSHIP_DESCENT, /* Lander / Starship landing approach (120 BPM driving aerospace rhythm) */
    MOON_CYBER_STARSHIP_LAUNCH,  /* Commercial Starship ascent / victory flight (150 BPM triumphant arpeggio) */
    MOON_CYBER_STATE_COUNT
} moon_cyber_state_t;

/* Telemetry snapshot passed to the cybernetic engine */
typedef struct {
    uint8_t   life_support_pct;   /* 0..100% health */
    int32_t   battery_pct;        /* 0..100% battery charge ratio */
    int32_t   power_net_kw;       /* Net kW generation delta */
    bool      is_night;           /* true during cryogenic lunar night */
    bool      power_brownout;     /* true if energy starved */
    bool      warning_o2;         /* true if low O2 */
    bool      warning_water;      /* true if low water */
    bool      warning_cold;       /* true if unheated freeze */
    bool      landing_active;     /* true during lander descent */
    uint8_t   starship_state;     /* moon_starship_state_t */
} moon_cyber_telemetry_t;

/* Lifecycle & Configuration */
void moon_cyber_init(void);
void moon_cyber_reset(void);
void moon_cyber_set_enabled(bool enabled);
bool moon_cyber_is_enabled(void);

/* Volume scaling: master volume [0..256] */
void moon_cyber_set_volume(int volume);
int  moon_cyber_get_volume(void);

/* Telemetry update: called every game frame */
void moon_cyber_update_telemetry(const moon_cyber_telemetry_t *telem);

/* Direct state override / query */
void moon_cyber_set_state(moon_cyber_state_t state);
moon_cyber_state_t moon_cyber_get_state(void);
int  moon_cyber_get_current_bpm(void);

/* Audio rendering: renders/mixes into 16-bit PCM buffer (accumulates with saturation) */
void moon_cyber_render(int16_t *buffer, int num_samples);

#endif /* MOON_CYBER_H */
