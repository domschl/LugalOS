/* user/moonbase/include/moon_ambient.h — Generative Lunar Space Ambient Synthesizer
 *
 * Minimal, meditative, generative space ambient soundscapes for Moonbase Lambda.
 * 100% pure integer and 16.16 fixed-point arithmetic (zero floating-point,
 * M-mode mstatus.FS = 0 compliant, freestanding C).
 */

#ifndef MOON_AMBIENT_H
#define MOON_AMBIENT_H

#include <stdint.h>
#include <stdbool.h>

#define MOON_AMBIENT_SAMPLE_RATE 16000

#if defined(CONFIG_BOARD_TAB5)
#define MOON_AMBIENT_DEFAULT_VOLUME 100
#elif defined(CONFIG_BOARD_ESP32P4)
#define MOON_AMBIENT_DEFAULT_VOLUME 60
#else
#define MOON_AMBIENT_DEFAULT_VOLUME 120
#endif

typedef enum {
    MOON_AMBIENT_MOOD_TRANQUIL = 0, /* Low root drone, celestial chimes, solar wind (default lunar surface) */
    MOON_AMBIENT_MOOD_SUBTERRANEAN, /* Deep volcanic tube hum, cavernous resonance */
    MOON_AMBIENT_MOOD_INDUSTRIAL,   /* Active base hum, harmonic fifth drone */
    MOON_AMBIENT_MOOD_COUNT
} moon_ambient_mood_t;

/* Initialize ambient synth state */
void moon_ambient_init(void);

/* Enable / disable ambient music generation */
void moon_ambient_set_enabled(bool enabled);
bool moon_ambient_is_enabled(void);

/* Set ambient volume [0..256] (default: 160) */
void moon_ambient_set_volume(int volume);
int  moon_ambient_get_volume(void);

/* Set current ambient mood */
void moon_ambient_set_mood(moon_ambient_mood_t mood);
moon_ambient_mood_t moon_ambient_get_mood(void);

/* Trigger audio ducking (e.g. on rocket launch / meteorite impact)
 * Temporarily reduces ambient volume by duck_amount [0..256] and smoothly recovers. */
void moon_ambient_duck(int duck_amount);

/* Synthesize and mix 16-bit signed mono ambient PCM samples directly into buffer.
 * If buffer already contains audio, ambient audio is summed with saturation clipping. */
void moon_ambient_render(int16_t *buffer, int num_samples);

#endif /* MOON_AMBIENT_H */
