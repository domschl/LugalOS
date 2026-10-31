/* user/ceres/include/ceres_cyber.h — Dynamic Cybernetic Voice & Meltdown Sequencer
 *
 * Implements a reactive 16-step procedural ostinato synthesizer voice that
 * dynamically adapts its tempo, rhythm, scale intervals, and pluck timbre
 * based on combat sector, fuel economy, survivor rescues, and base meltdown telemetry.
 *
 * Supports three generative variation systems:
 * 1. 4-bar phrasing with dynamic turnaround fills (A -> A -> B -> Fill)
 * 2. Contextual telemetry micro-modulations (thruster boost, pitch bank, rescue streak)
 * 3. Procedural micro-ornamentation (deterministic LFSR grace notes, octave sparkles, duty morphs)
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#ifndef CERES_CYBER_H
#define CERES_CYBER_H

#include <stdint.h>
#include <stdbool.h>

#define CERES_CYBER_SAMPLE_RATE 16000

/* Telemetry states governing the Ceres Alpha cybernetic voice */
typedef enum {
    CERES_CYBER_PATROL = 0,     /* Title screen / calm cruising (108 BPM laboratory pulse) */
    CERES_CYBER_COMBAT,         /* Sectors 1-3: Cavern & trench combat runner (128 BPM minor drive) */
    CERES_CYBER_STRONGHOLD,     /* Sectors 4-6: Fortified base / Labyrinth / Stronghold (140 BPM Phrygian) */
    CERES_CYBER_SURVIVOR,       /* Survivor in range / tractor active / parachute rescue (120 BPM sonar beacon) */
    CERES_CYBER_LOW_FUEL,       /* Low fuel alert <20% (75 BPM sputtering broken meter) */
    CERES_CYBER_MELTDOWN,       /* Sector 6 Reactor Core detonated - emergency sprint (175 BPM chromatic alarm) */
    CERES_CYBER_STATE_COUNT
} ceres_cyber_state_t;

/* Telemetry snapshot passed to the cybernetic engine */
typedef struct {
    int       sector;             /* Current sector index (1..6) */
    int32_t   fuel;               /* Fuel level: 0..1000 */
    bool      low_fuel_alert;     /* Fuel < 20% alert active */
    bool      alive;              /* Player ship is operational */
    bool      tractor_active;     /* Tractor scoop actively locked on survivor */
    bool      survivor_visible;   /* Survivor or parachute on active screen */
    uint16_t  meltdown_timer;     /* Sector 6 base meltdown escape frames */
    bool      game_active;        /* True when in CERES_STATE_PLAYING */
    int8_t    thrust_boost;       /* -1 = brake/left, 0 = normal, +1 = forward/boost */
    int8_t    pitch_bank;         /* -1 = dive/nose-down, 0 = level, +1 = climb/nose-up */
    bool      rescue_event;       /* Triggered when survivor collected */
} ceres_cyber_telemetry_t;

/* Lifecycle & Configuration */
void ceres_cyber_init(void);
void ceres_cyber_reset(void);
void ceres_cyber_set_enabled(bool enabled);
bool ceres_cyber_is_enabled(void);

/* Volume scaling: master volume [0..256] */
void ceres_cyber_set_volume(int volume);
int  ceres_cyber_get_volume(void);

/* Telemetry update: called every game frame */
void ceres_cyber_update_telemetry(const ceres_cyber_telemetry_t *telem);

/* Direct state override / query */
void ceres_cyber_set_state(ceres_cyber_state_t state);
ceres_cyber_state_t ceres_cyber_get_state(void);
int  ceres_cyber_get_current_bpm(void);
uint8_t ceres_cyber_get_bar_index(void);

/* Audio rendering: renders/mixes into 16-bit PCM buffer (accumulates with saturation) */
void ceres_cyber_render(int16_t *buffer, int num_samples);

#endif /* CERES_CYBER_H */
