/* user/ceres/include/ceres_sound.h — Chiptune Audio Synthesizer
 *
 * Lightweight procedural sound effects generator with zero floating-point math
 * (100% fixed-point and integer arithmetic, safe for M-mode mstatus.FS = 0).
 * Supports pulse wave, triangle wave, and LFSR pseudo-random noise channels.
 */

#ifndef CERES_SOUND_H
#define CERES_SOUND_H

#include <stdint.h>
#include <stdbool.h>

#define CERES_AUDIO_SAMPLE_RATE 22050

typedef enum {
    CERES_SFX_NONE = 0,
    CERES_SFX_LASER,           /* Fast downward frequency chirp */
    CERES_SFX_BOMB_DROP,       /* Descending whistle */
    CERES_SFX_EXPLOSION_TINY,  /* Spark pop / bullet ricochet */
    CERES_SFX_EXPLOSION_MED,   /* Ground target explosion */
    CERES_SFX_EXPLOSION_HUGE,  /* Reactor core blast */
    CERES_SFX_RESCUE_CHIME,    /* Ascending arpeggio */
    CERES_SFX_HEROIC_CHIME,    /* Heroic fanfare */
    CERES_SFX_LOW_FUEL,        /* Urgent warning beeps */
    CERES_SFX_BARRIER_DOWN,    /* Electric arc discharge */
    CERES_SFX_SIREN_PULSE,     /* Meltdown strobe tone */
    CERES_SFX_COUNT
} ceres_sfx_id_t;

/* Initialize the synthesizer engine */
void ceres_sound_init(void);

/* Trigger a sound effect */
void ceres_sound_play(ceres_sfx_id_t sfx);

/* Synthesize 16-bit signed mono PCM samples into buffer */
void ceres_sound_render(int16_t *buffer, int num_samples);

/* Enable / disable sound output */
void ceres_sound_set_enabled(bool enabled);
bool ceres_sound_is_enabled(void);

#endif /* CERES_SOUND_H */
