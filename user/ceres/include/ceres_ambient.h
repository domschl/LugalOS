/* user/ceres/include/ceres_ambient.h — Generative Space Ambient Synthesizer
 *
 * Minimal, meditative, generative space ambient soundscapes.
 * 100% pure integer and 16.16 fixed-point arithmetic (zero floating-point,
 * M-mode mstatus.FS = 0 compliant, freestanding C).
 */

#ifndef CERES_AMBIENT_H
#define CERES_AMBIENT_H

#include <stdint.h>
#include <stdbool.h>

#define CERES_AMBIENT_SAMPLE_RATE 16000

#if defined(CONFIG_BOARD_TAB5)
#define CERES_AMBIENT_DEFAULT_VOLUME 100
#elif defined(CONFIG_BOARD_ESP32P4)
#define CERES_AMBIENT_DEFAULT_VOLUME 60
#else
#define CERES_AMBIENT_DEFAULT_VOLUME 120
#endif

typedef enum {
    CERES_AMBIENT_MOOD_DEEP_SPACE = 0, /* Low root drone, celestial chimes, solar wind (default) */
    CERES_AMBIENT_MOOD_CAVERN,         /* Subterranean resonant hum, damp harmonic echoes */
    CERES_AMBIENT_MOOD_TENSE,          /* Higher tension minor drone, subtle warning resonance */
    CERES_AMBIENT_MOOD_COUNT
} ceres_ambient_mood_t;

/* Initialize ambient synth state */
void ceres_ambient_init(void);

/* Enable / disable ambient music generation */
void ceres_ambient_set_enabled(bool enabled);
bool ceres_ambient_is_enabled(void);

/* Set ambient volume [0..256] (default: 160) */
void ceres_ambient_set_volume(int volume);
int  ceres_ambient_get_volume(void);

/* Set current ambient mood */
void ceres_ambient_set_mood(ceres_ambient_mood_t mood);
ceres_ambient_mood_t ceres_ambient_get_mood(void);

/* West Coast Buchla wavefolder drive [256..1024]
 * 256 = clean pure sine bypass (1.0x), 512 = 1st fold (2.0x), 768 = 2nd fold (3.0x), 1024 = heavy fold (4.0x) */
void ceres_ambient_set_wavefold(int fold_drive);
int  ceres_ambient_get_wavefold(void);

/* Trigger audio ducking (e.g. on loud explosions / core blast)
 * Temporarily reduces ambient volume by duck_amount [0..256] and smoothly recovers. */
void ceres_ambient_duck(int duck_amount);

/* Synthesize and mix 16-bit signed mono ambient PCM samples directly into buffer.
 * If buffer already contains SFX audio, ambient audio is summed with saturation clipping. */
void ceres_ambient_render(int16_t *buffer, int num_samples);

#endif /* CERES_AMBIENT_H */
